//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.md file in the project root for full license information.
//
// usp_tts_engine_adapter.cpp: Implementation definitions for CSpxMockTtsEngineAdapter C++ class
//

#include "stdafx.h"
#include <math.h>
#include "synthesis_helper.h"
#include "usp_tts_engine_adapter.h"
#include "create_object_helpers.h"
#include "guid_utils.h"
#include "handle_table.h"
#include "service_helpers.h"
#include "shared_ptr_helpers.h"
#include "property_bag_impl.h"
#include "property_id_2_name_map.h"
#include "json.h"
#include "spx_build_information.h"
#include "time_utils.h"

#define SPX_DBG_TRACE_USP_TTS 0


namespace Microsoft {
namespace CognitiveServices {
namespace Speech {
namespace Impl {


CSpxUspTtsEngineAdapter::CSpxUspTtsEngineAdapter()
{
    SPX_DBG_TRACE_VERBOSE_IF(SPX_DBG_TRACE_USP_TTS, __FUNCTION__);
}

CSpxUspTtsEngineAdapter::~CSpxUspTtsEngineAdapter()
{
    SPX_DBG_TRACE_VERBOSE_IF(SPX_DBG_TRACE_USP_TTS, __FUNCTION__);
    Term();
}

void CSpxUspTtsEngineAdapter::Init()
{
    // Initialize thread service
    m_threadService = SpxCreateObjectWithSite<ISpxThreadService>("CSpxThreadService", SpxQueryInterface<ISpxGenericSite>(GetSite()));

    // Get proxy setting
    GetProxySetting();

    // Initialize websocket platform
    Microsoft::CognitiveServices::Speech::USP::PlatformInit(m_proxyHost.data(), m_proxyPort, m_proxyUsername.data(), m_proxyPassword.data());

    // Initialize authentication related information

    std::string endpoint = ISpxPropertyBagImpl::GetStringValue(GetPropertyName(PropertyId::SpeechServiceConnection_Endpoint), "");
    std::string region = ISpxPropertyBagImpl::GetStringValue(GetPropertyName(PropertyId::SpeechServiceConnection_Region), "");
    std::string subscriptionKey = ISpxPropertyBagImpl::GetStringValue(GetPropertyName(PropertyId::SpeechServiceConnection_Key), "");

    if (!endpoint.empty() && !CSpxSynthesisHelper::IsCustomVoiceEndpoint(endpoint) && !CSpxSynthesisHelper::IsStandardVoiceEndpoint(endpoint))
    {
        // Scenario 1, custom endpoint (e.g. on prem), no need authentication
        m_endpoint = endpoint;
    }
    else if (!endpoint.empty() && CSpxSynthesisHelper::IsCustomVoiceEndpoint(endpoint))
    {
        // Scenario 2, custom voice, need authentication (and therefore need initialize m_authenticator)
        m_endpoint = endpoint;
        region = CSpxSynthesisHelper::ParseRegionFromCognitiveServiceEndpoint(endpoint);

        // Construct cognitive service token issue URL based on region
        auto issueTokenUrl = std::string(HTTPS_URL_PREFIX) + region + ISSUE_TOKEN_HOST_SUFFIX + ISSUE_TOKEN_URL_PATH;

        m_authenticator = std::make_shared<CSpxRestTtsAuthenticator>(issueTokenUrl, subscriptionKey, m_proxyHost, m_proxyPort, m_proxyUsername, m_proxyPassword);
    }
    else if ((endpoint.empty() && !region.empty()) || (!endpoint.empty() && CSpxSynthesisHelper::IsStandardVoiceEndpoint(endpoint)))
    {
        // Scenario 3, standard voice, need issue token (and therefore need initialize m_authenticator)
        if (endpoint.empty() && !region.empty())
        {
            // Construct standard voice endpoint based on region
            m_endpoint = std::string(WSS_URL_PREFIX) + region + TTS_COGNITIVE_SERVICE_HOST_SUFFIX + TTS_COGNITIVE_SERVICE_WSS_URL_PATH;
        }

        if (!endpoint.empty() && CSpxSynthesisHelper::IsStandardVoiceEndpoint(endpoint))
        {
            m_endpoint = endpoint;
            region = CSpxSynthesisHelper::ParseRegionFromCognitiveServiceEndpoint(endpoint);
        }

        // Construct cognitive service token issue URL based on region
        auto issueTokenUrl = std::string(HTTPS_URL_PREFIX) + region + ISSUE_TOKEN_HOST_SUFFIX + ISSUE_TOKEN_URL_PATH;

        m_authenticator = std::make_shared<CSpxRestTtsAuthenticator>(issueTokenUrl, subscriptionKey, m_proxyHost, m_proxyPort, m_proxyUsername, m_proxyPassword);
    }
    else
    {
        ThrowRuntimeError("Invalid combination of endpoint, region and(or) subscription key.");
    }
}

void CSpxUspTtsEngineAdapter::Term()
{
    UspTerminate();
    SpxQueryInterface<ISpxObjectInit>(m_threadService)->Term();
}

void CSpxUspTtsEngineAdapter::SetOutput(std::shared_ptr<ISpxAudioOutput> output)
{
    SPX_DBG_TRACE_VERBOSE_IF(SPX_DBG_TRACE_USP_TTS, __FUNCTION__);
    m_audioOutput = output;
}

std::shared_ptr<ISpxSynthesisResult> CSpxUspTtsEngineAdapter::Speak(const std::string& text, bool isSsml, const std::wstring& requestId)
{
    SPX_DBG_TRACE_VERBOSE_IF(SPX_DBG_TRACE_USP_TTS, __FUNCTION__);
    SPX_DBG_ASSERT(UspState::Idle == m_uspState || UspState::Error == m_uspState);

    auto ssml = text;
    if (!isSsml)
    {
        auto language = ISpxPropertyBagImpl::GetStringValue(GetPropertyName(PropertyId::SpeechServiceConnection_SynthLanguage), "");
        auto voice = ISpxPropertyBagImpl::GetStringValue(GetPropertyName(PropertyId::SpeechServiceConnection_SynthVoice), "");
        ssml = CSpxSynthesisHelper::BuildSsml(text, language, voice);
    }

    SPX_DBG_TRACE_VERBOSE("SSML sent to TTS cognitive service: %s", ssml.data());

    EnsureUspConnection();

    // Set initial values for current utterance
    m_currentRequestId = requestId;
    m_currentText = PAL::ToWString(text);
    m_currentTextIsSsml = isSsml;
    m_currentTextOffset = 0;
    m_currentErrorCode = static_cast<USP::ErrorCode>(0);
    m_currentErrorMessage = std::string();

    // Send request
    m_uspState = UspState::Sending;
    UspSendSynthesisContext();
    UspSendSsml(ssml);

    std::unique_lock<std::mutex> lock(m_mutex);
#ifdef _DEBUG
    while (!m_cv.wait_for(lock, std::chrono::milliseconds(100), [&] { return m_uspState == UspState::Idle || m_uspState == UspState::Error; }))
    {
        SPX_DBG_TRACE_VERBOSE("%s: waiting for USP to finish receiving data ...", __FUNCTION__);
    }
#else
    m_cv.wait(lock, [&] { return m_uspState == UspState::Idle || m_uspState == UspState::Error; });
#endif

    bool hasHeader = false;
    auto outputFormat = GetOutputFormat(m_audioOutput, &hasHeader);

    auto result = SpxCreateObjectWithSite<ISpxSynthesisResult>("CSpxSynthesisResult", SpxQueryInterface<ISpxGenericSite>(GetSite()));
    auto resultInit = SpxQueryInterface<ISpxSynthesisResultInit>(result);
    if (m_uspState == UspState::Error)
    {
        resultInit->InitSynthesisResult(requestId, ResultReason::Canceled,
            CancellationReason::Error, UspErrorCodeToCancellationErrorCode(m_currentErrorCode),
            nullptr, 0, outputFormat.get(), hasHeader);
        SpxQueryInterface<ISpxNamedProperties>(resultInit)->SetStringValue(GetPropertyName(PropertyId::CancellationDetails_ReasonDetailedText), m_currentErrorMessage.data());
    }
    else
    {
        resultInit->InitSynthesisResult(requestId, ResultReason::SynthesizingAudioCompleted,
            REASON_CANCELED_NONE, CancellationErrorCode::NoError, m_currentReceivedData.data(), m_currentReceivedData.size(), outputFormat.get(), hasHeader);
    }

    return result;
}

std::shared_ptr<ISpxNamedProperties> CSpxUspTtsEngineAdapter::GetParentProperties() const
{
    return SpxQueryService<ISpxNamedProperties>(GetSite());
}

void CSpxUspTtsEngineAdapter::GetProxySetting()
{
    m_proxyHost = ISpxPropertyBagImpl::GetStringValue(GetPropertyName(PropertyId::SpeechServiceConnection_ProxyHostName), "");
    m_proxyPort = std::stoi(ISpxPropertyBagImpl::GetStringValue(GetPropertyName(PropertyId::SpeechServiceConnection_ProxyPort), "0"));
    if (m_proxyPort < 0)
    {
        ThrowInvalidArgumentException("Invalid proxy port: %d", m_proxyPort);
    }

    m_proxyUsername = ISpxPropertyBagImpl::GetStringValue(GetPropertyName(PropertyId::SpeechServiceConnection_ProxyUserName), "");
    m_proxyPassword = ISpxPropertyBagImpl::GetStringValue(GetPropertyName(PropertyId::SpeechServiceConnection_ProxyPassword), "");
}

void CSpxUspTtsEngineAdapter::SetSpeechConfigMessage()
{
    constexpr auto systemName = "SpeechSDK";

    nlohmann::json speechConfig;

    // Set system configuration data.
    speechConfig["context"]["system"]["version"] = BuildInformation::g_fullVersion;
    speechConfig["context"]["system"]["name"] = systemName;
    speechConfig["context"]["system"]["build"] = BuildInformation::g_buildPlatform;

    // Set OS configuration data.
    auto osInfo = PAL::getOperatingSystem();
    speechConfig["context"]["os"]["platform"] = osInfo.platform;
    speechConfig["context"]["os"]["name"] = osInfo.name;
    speechConfig["context"]["os"]["version"] = osInfo.version;

    m_speechConfig = speechConfig.dump();
}

void CSpxUspTtsEngineAdapter::UspSendSpeechConfig()
{
    constexpr auto messagePath = "speech.config";
    SPX_DBG_TRACE_VERBOSE("%s %s", messagePath, m_speechConfig.c_str());
    UspSendMessage(messagePath, m_speechConfig, USP::MessageType::Config);
}

void CSpxUspTtsEngineAdapter::UspSendSynthesisContext()
{
    constexpr auto messagePath = "synthesis.context";

    // Set synthesis context data.
    nlohmann::json synthesisContext;
    synthesisContext["synthesis"]["audio"]["outputFormat"] = GetOutputFormatString(m_audioOutput);
    synthesisContext["synthesis"]["audio"]["metadataOptions"]["wordBoundaryEnabled"] = ISpxPropertyBagImpl::GetStringValue("SpeechServiceResponse_Synthesis_WordBoundaryEnabled", "true");
    synthesisContext["synthesis"]["audio"]["metadataOptions"]["sentenceBoundaryEnabled"] = ISpxPropertyBagImpl::GetStringValue("SpeechServiceResponse_Synthesis_SentenceBoundaryEnabled", "false");

    UspSendMessage(messagePath, synthesisContext.dump(), USP::MessageType::Context);
}

void CSpxUspTtsEngineAdapter::UspSendSsml(const std::string& ssml)
{
    constexpr auto messagePath = "ssml";
    SPX_DBG_TRACE_VERBOSE("%s %s", messagePath, ssml.c_str());
    UspSendMessage(messagePath, ssml, USP::MessageType::Ssml);
}

void CSpxUspTtsEngineAdapter::UspSendMessage(const std::string& messagePath, const std::string &buffer, USP::MessageType messageType)
{
    SPX_DBG_TRACE_VERBOSE("%s='%s'", messagePath.c_str(), buffer.c_str());
    UspSendMessage(messagePath, (const uint8_t*)buffer.c_str(), buffer.length(), messageType);
}

void CSpxUspTtsEngineAdapter::UspSendMessage(const std::string& messagePath, const uint8_t* buffer, size_t size, USP::MessageType messageType)
{
    SPX_DBG_ASSERT(m_uspConnection != nullptr);
    if (m_uspConnection != nullptr)
    {
        m_uspConnection->SendMessage(messagePath, buffer, size, messageType);
    }
}

void CSpxUspTtsEngineAdapter::EnsureUspConnection()
{
    if (m_uspConnection == nullptr)
    {
        UspInitialize();
    }
    else if (!m_uspConnection->IsConnected())
    {
        // If the connection was closed due to any reason, we re-connect
        UspTerminate();
        UspInitialize();
    }
    else if (PAL::GetTicks(std::chrono::system_clock::now() - m_lastConnectTime) > (uint64_t)9 * 60 * 1000 * 10000)
    {
        // Per https://speechwiki.azurewebsites.net/partners/protocol-websockets-tts.html#connection-duration-limitations
        // The service closes the active connect after 10 mins.
        // We re-connect it after 9 mins in case it breaks an on-going speak.
        UspTerminate();
        UspInitialize();
    }
}

void CSpxUspTtsEngineAdapter::UspInitialize()
{
    SPX_DBG_TRACE_VERBOSE("%s: this=0x%8p", __FUNCTION__, (void*)this);
    SPX_IFTRUE_THROW_HR(m_uspConnection != nullptr, SPXERR_ALREADY_INITIALIZED);

    // Fill authorization token
    std::array<std::string, static_cast<size_t>(USP::AuthenticationType::SIZE_AUTHENTICATION_TYPE)> authData;
    if (m_authenticator.get() != nullptr && !m_authenticator->GetAccessToken().empty())
    {
        authData[static_cast<size_t>(USP::AuthenticationType::AuthorizationToken)] = m_authenticator->GetAccessToken();
    }
    else
    {
        authData[static_cast<size_t>(USP::AuthenticationType::AuthorizationToken)] = ISpxPropertyBagImpl::GetStringValue(GetPropertyName(PropertyId::SpeechServiceAuthorization_Token), "");
    }

    // Create the usp client, which we'll configure and use to create the actual connection
    auto uspCallbacks = SpxCreateObjectWithSite<ISpxUspCallbacks>("CSpxUspCallbackWrapper", this);
    auto client = USP::Client(uspCallbacks, USP::EndpointType::SpeechSynthesis, PAL::CreateGuidWithoutDashes(), m_threadService)
        .SetAuthentication(authData)
        .SetEndpointUrl(m_endpoint);

    // Set proxy
    if (!m_proxyHost.empty() && m_proxyPort > 0)
    {
        client.SetProxyServerInfo(m_proxyHost.data(), m_proxyPort, m_proxyUsername.data(), m_proxyPassword.data());
    }

    // Try to connect
    USP::ConnectionPtr uspConnection;

    try
    {
        uspConnection = client.Connect();
    }
    catch (const std::exception& e)
    {
        SPX_TRACE_ERROR("Error: '%s'", e.what());
        OnError(true, USP::ErrorCode::ConnectionError, e.what());
    }
    catch (...)
    {
        SPX_TRACE_ERROR("Error: Unexpected exception in UspInitialize");
        OnError(true, USP::ErrorCode::ConnectionError, "Error: Unexpected exception in UspInitialize");
    }

    // if error occurs in the above client.Connect, return.
    if (uspConnection == nullptr)
    {
        return;
    }

    // We're done!!
    m_uspCallbacks = uspCallbacks;
    m_uspConnection = std::move(uspConnection);
    m_uspState = UspState::Idle;
    m_lastConnectTime = std::chrono::system_clock::now();

    // Send speech config message
    if (m_uspConnection != nullptr)
    {
        ISpxPropertyBagImpl::SetStringValue(GetPropertyName(PropertyId::SpeechServiceConnection_Url), m_uspConnection->GetConnectionUrl().c_str());

        // Construct config message payload
        SetSpeechConfigMessage();

        // Send speech config
        UspSendSpeechConfig();
    }
}

void CSpxUspTtsEngineAdapter::UspTerminate()
{
    // Term the callbacks first and then reset/release the connection
    SpxTermAndClear(m_uspCallbacks);

    if (m_uspConnection != nullptr)
    {
        m_uspConnection.reset();
        m_uspConnection = nullptr;
    }
}

void CSpxUspTtsEngineAdapter::OnTurnStart(const USP::TurnStartMsg& message)
{
    UNUSED(message);
    std::unique_lock<std::mutex> lock(m_mutex);
    m_uspState = UspState::Receiving;
    m_currentReceivedData.clear();
}

void CSpxUspTtsEngineAdapter::OnAudioOutputChunk(const USP::AudioOutputChunkMsg& message)
{
    InvokeOnSite([this, message](const SitePtr& p) {
        if (message.audioLength > 0)
        {
            p->Write(this, m_currentRequestId, (uint8_t *)message.audioBuffer, (uint32_t)message.audioLength);
        }
    });

    std::unique_lock<std::mutex> lock(m_mutex);
    auto originalSize = m_currentReceivedData.size();
    m_currentReceivedData.resize(originalSize + message.audioLength);
    memcpy(m_currentReceivedData.data() + originalSize, message.audioBuffer, message.audioLength);
}

void CSpxUspTtsEngineAdapter::OnAudioOutputMetadata(const USP::AudioOutputMetadataMsg& message)
{
    auto synthesizerEvents = SpxQueryInterface<ISpxSynthesizerEvents>(GetSite());

    auto iterator = message.metadatas.begin();
    while (iterator != message.metadatas.end())
    {
        if (PAL::stricmp(iterator->type.data(), METADATA_TYPE_WORD_BOUNDARY) == 0)
        {
            auto wordBoundary = iterator->textBoundary;
            auto textOffset = m_currentText.find(wordBoundary.text.data(), m_currentTextOffset);

            if (m_currentTextIsSsml)
            {
                while (textOffset != std::string::npos && InSsmlTag(textOffset, m_currentText, m_currentTextOffset))
                {
                    textOffset = m_currentText.find(wordBoundary.text.data(), m_currentTextOffset);
                }
            }

            if (textOffset != std::string::npos)
            {
                m_currentTextOffset = (uint32_t)textOffset + (uint32_t)wordBoundary.text.length();
                synthesizerEvents->FireWordBoundary(wordBoundary.audioOffset, (uint32_t)textOffset, (uint32_t)wordBoundary.text.length());
            }
        }

        iterator++;
    }
}

void CSpxUspTtsEngineAdapter::OnTurnEnd(const USP::TurnEndMsg& message)
{
    UNUSED(message);
    std::unique_lock<std::mutex> lock(m_mutex);
    m_uspState = UspState::Idle;
    m_cv.notify_all();
}

void CSpxUspTtsEngineAdapter::OnError(bool transport, USP::ErrorCode errorCode, const std::string& errorMessage)
{
    UNUSED(transport);
    SPX_DBG_TRACE_VERBOSE("Response: On Error: Code:%d, Message: %s.\n", errorCode, errorMessage.c_str());
    std::unique_lock<std::mutex> lock(m_mutex);
    m_currentErrorCode = errorCode;
    m_currentErrorMessage = errorMessage;
    m_currentErrorMessage += ". USP state: " + CSpxSynthesisHelper::itos((int)(UspState)m_uspState) + ".";
    m_currentErrorMessage += " Received audio size: " + CSpxSynthesisHelper::itos(m_currentReceivedData.size()) + "bytes.";
    m_uspState = UspState::Error;
    m_cv.notify_all();
}

SpxWAVEFORMATEX_Type CSpxUspTtsEngineAdapter::GetOutputFormat(std::shared_ptr<ISpxAudioOutput> output, bool* hasHeader)
{
    auto audioStream = SpxQueryInterface<ISpxAudioStream>(output);
    auto requiredFormatSize = audioStream->GetFormat(nullptr, 0);
    auto format = SpxAllocWAVEFORMATEX(requiredFormatSize);
    audioStream->GetFormat(format.get(), requiredFormatSize);

    if (hasHeader != nullptr)
    {
        *hasHeader = SpxQueryInterface<ISpxAudioOutputFormat>(output)->HasHeader();
    }

    return format;
}

std::string CSpxUspTtsEngineAdapter::GetOutputFormatString(std::shared_ptr<ISpxAudioOutput> output)
{
    auto outputFormat = SpxQueryInterface<ISpxAudioOutputFormat>(output);
    auto formatString = outputFormat->GetFormatString();
    if (outputFormat->HasHeader())
    {
        formatString = outputFormat->GetRawFormatString();
    }

    return formatString;
}

bool CSpxUspTtsEngineAdapter::InSsmlTag(size_t currentPos, const std::wstring& ssml, size_t beginningPos)
{
    if (currentPos < beginningPos || currentPos >= ssml.length() || beginningPos >= ssml.length())
    {
        return false;
    }

    auto pos = currentPos;
    while (pos > beginningPos)
    {
        if (*(ssml.data() + pos) == '>')
        {
            return false;
        }

        if (*(ssml.data() + pos) == '<')
        {
            return true;
        }

        pos--;
    }

    return false;
}

CancellationErrorCode CSpxUspTtsEngineAdapter::UspErrorCodeToCancellationErrorCode(USP::ErrorCode uspErrorCode)
{
    std::map<USP::ErrorCode, CancellationErrorCode> uspErrorCodeToCancellationErrorCodeMapping = {
        { USP::ErrorCode::AuthenticationError, CancellationErrorCode::AuthenticationFailure },
        { USP::ErrorCode::BadRequest, CancellationErrorCode::BadRequest },
        { USP::ErrorCode::ConnectionError, CancellationErrorCode::ConnectionFailure },
        { USP::ErrorCode::Forbidden, CancellationErrorCode::Forbidden },
        { USP::ErrorCode::RuntimeError, CancellationErrorCode::RuntimeError },
        { USP::ErrorCode::ServiceError, CancellationErrorCode::ServiceError },
        { USP::ErrorCode::ServiceUnavailable, CancellationErrorCode::ServiceUnavailable },
        { USP::ErrorCode::TooManyRequests, CancellationErrorCode::TooManyRequests }
    };

    auto cancellationErrorCode = CancellationErrorCode::NoError;

    auto iter = uspErrorCodeToCancellationErrorCodeMapping.find(uspErrorCode);
    if (iter != uspErrorCodeToCancellationErrorCodeMapping.end())
    {
        cancellationErrorCode = iter->second;
    }

    return cancellationErrorCode;
}


} } } } // Microsoft::CognitiveServices::Speech::Impl
