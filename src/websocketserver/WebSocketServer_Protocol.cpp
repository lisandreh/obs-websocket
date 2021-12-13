/*
obs-websocket
Copyright (C) 2016-2021 Stephane Lepin <stephane.lepin@gmail.com>
Copyright (C) 2020-2021 Kyle Manning <tt2468@gmail.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include <obs-module.h>

#include "WebSocketServer.h"
#include "../requesthandler/RequestHandler.h"
#include "../eventhandler/EventHandler.h"
#include "../obs-websocket.h"
#include "../Config.h"
#include "../utils/Crypto.h"
#include "../utils/Platform.h"
#include "../utils/Compat.h"

static bool IsSupportedRpcVersion(uint8_t requestedVersion)
{
	return (requestedVersion == 1);
}

void WebSocketServer::SetSessionParameters(SessionPtr session, ProcessResult &ret, const json &payloadData)
{
	if (payloadData.contains("ignoreInvalidMessages")) {
		if (!payloadData["ignoreInvalidMessages"].is_boolean()) {
			ret.closeCode = WebSocketCloseCode::InvalidDataFieldType;
			ret.closeReason = "Your `ignoreInvalidMessages` is not a boolean.";
			return;
		}
		session->SetIgnoreInvalidMessages(payloadData["ignoreInvalidMessages"]);
	}

	if (payloadData.contains("eventSubscriptions")) {
		if (!payloadData["eventSubscriptions"].is_number_unsigned()) {
			ret.closeCode = WebSocketCloseCode::InvalidDataFieldType;
			ret.closeReason = "Your `eventSubscriptions` is not an unsigned number.";
			return;
		}
		session->SetEventSubscriptions(payloadData["eventSubscriptions"]);
	}
}

void WebSocketServer::ProcessMessage(SessionPtr session, WebSocketServer::ProcessResult &ret, WebSocketOpCode::WebSocketOpCode opCode, json &payloadData)
{
	if (!payloadData.is_object()) {
		if (payloadData.is_null()) {
			ret.closeCode = WebSocketCloseCode::MissingDataField;
			ret.closeReason = "Your payload is missing data (`d`).";
		} else {
			ret.closeCode = WebSocketCloseCode::InvalidDataFieldType;
			ret.closeReason = "Your payload's data (`d`) is not an object.";
		}
		return;
	}

	// Only `Identify` is allowed when not identified
	if (!session->IsIdentified() && opCode != 1) {
		ret.closeCode = WebSocketCloseCode::NotIdentified;
		ret.closeReason = "You attempted to send a non-Identify message while not identified.";
		return;
	}

	switch (opCode) {
		case WebSocketOpCode::Identify: { // Identify
			std::unique_lock<std::mutex> sessionLock(session->OperationMutex);
			if (session->IsIdentified()) {
				if (!session->IgnoreInvalidMessages()) {
					ret.closeCode = WebSocketCloseCode::AlreadyIdentified;
					ret.closeReason = "You are already Identified with the obs-websocket server.";
				}
				return;
			}

			if (session->AuthenticationRequired()) {
				if (!payloadData.contains("authentication")) {
					ret.closeCode = WebSocketCloseCode::AuthenticationFailed;
					ret.closeReason = "Your payload's data is missing an `authentication` string, however authentication is required.";
					return;
				}
				if (!Utils::Crypto::CheckAuthenticationString(session->Secret(), session->Challenge(), payloadData["authentication"])) {
					auto conf = GetConfig();
					if (conf && conf->AlertsEnabled) {
						QString title = obs_module_text("OBSWebSocket.TrayNotification.AuthenticationFailed.Title");
						QString body = QString(obs_module_text("OBSWebSocket.TrayNotification.AuthenticationFailed.Body")).arg(QString::fromStdString(session->RemoteAddress()));
						Utils::Platform::SendTrayNotification(QSystemTrayIcon::Warning, title, body);
					}
					ret.closeCode = WebSocketCloseCode::AuthenticationFailed;
					ret.closeReason = "Authentication failed.";
					return;
				}
			}

			if (!payloadData.contains("rpcVersion")) {
				ret.closeCode = WebSocketCloseCode::MissingDataField;
				ret.closeReason = "Your payload's data is missing an `rpcVersion`.";
				return;
			}

			if (!payloadData["rpcVersion"].is_number_unsigned()) {
				ret.closeCode = WebSocketCloseCode::InvalidDataFieldType;
				ret.closeReason = "Your `rpcVersion` is not an unsigned number.";
			}

			uint8_t requestedRpcVersion = payloadData["rpcVersion"];
			if (!IsSupportedRpcVersion(requestedRpcVersion)) {
				ret.closeCode = WebSocketCloseCode::UnsupportedRpcVersion;
				ret.closeReason = "Your requested RPC version is not supported by this server.";
				return;
			}
			session->SetRpcVersion(requestedRpcVersion);

			SetSessionParameters(session, ret, payloadData);
			if (ret.closeCode != WebSocketCloseCode::DontClose) {
				return;
			}

			// Increment refs for event subscriptions
			auto eventHandler = GetEventHandler();
			eventHandler->ProcessSubscription(session->EventSubscriptions());

			// Mark session as identified
			session->SetIsIdentified(true);

			// Send desktop notification. TODO: Move to UI code
			auto conf = GetConfig();
			if (conf && conf->AlertsEnabled) {
				QString title = obs_module_text("OBSWebSocket.TrayNotification.Identified.Title");
				QString body = QString(obs_module_text("OBSWebSocket.TrayNotification.Identified.Body")).arg(QString::fromStdString(session->RemoteAddress()));
				Utils::Platform::SendTrayNotification(QSystemTrayIcon::Information, title, body);
			}

			ret.result["op"] = WebSocketOpCode::Identified;
			ret.result["d"]["negotiatedRpcVersion"] = session->RpcVersion();
			} return;
		case WebSocketOpCode::Reidentify: { // Reidentify
			std::unique_lock<std::mutex> sessionLock(session->OperationMutex);

			// Decrement refs for current subscriptions
			auto eventHandler = GetEventHandler();
			eventHandler->ProcessUnsubscription(session->EventSubscriptions());

			SetSessionParameters(session, ret, payloadData);
			if (ret.closeCode != WebSocketCloseCode::DontClose) {
				return;
			}

			// Increment refs for new subscriptions
			eventHandler->ProcessSubscription(session->EventSubscriptions());

			ret.result["op"] = WebSocketOpCode::Identified;
			ret.result["d"]["negotiatedRpcVersion"] = session->RpcVersion();
			} return;
		case WebSocketOpCode::Request: { // Request
			// RequestID checking has to be done here where we are able to close the connection.
			if (!payloadData.contains("requestId")) {
				if (!session->IgnoreInvalidMessages()) {
					ret.closeCode = WebSocketCloseCode::MissingDataField;
					ret.closeReason = "Your payload data is missing a `requestId`.";
				}
				return;
			}

			RequestHandler requestHandler(session);
			Request request(payloadData["requestType"], payloadData["requestData"]);

			RequestResult requestResult = requestHandler.ProcessRequest(request);

			json resultPayloadData;
			resultPayloadData["requestType"] = payloadData["requestType"];
			resultPayloadData["requestId"] = payloadData["requestId"];
			resultPayloadData["requestStatus"] = {
				{"result", requestResult.StatusCode == RequestStatus::Success},
				{"code", requestResult.StatusCode}
			};
			if (!requestResult.Comment.empty())
				resultPayloadData["requestStatus"]["comment"] = requestResult.Comment;
			if (requestResult.ResponseData.is_object())
				resultPayloadData["responseData"] = requestResult.ResponseData;
			ret.result["op"] = WebSocketOpCode::RequestResponse;
			ret.result["d"] = resultPayloadData;
			} return;
		case WebSocketOpCode::RequestBatch: { // RequestBatch
			// RequestID checking has to be done here where we are able to close the connection.
			if (!payloadData.contains("requestId")) {
				if (!session->IgnoreInvalidMessages()) {
					ret.closeCode = WebSocketCloseCode::MissingDataField;
					ret.closeReason = "Your payload data is missing a `requestId`.";
				}
				return;
			}

			if (!payloadData.contains("requests")) {
				if (!session->IgnoreInvalidMessages()) {
					ret.closeCode = WebSocketCloseCode::MissingDataField;
					ret.closeReason = "Your payload data is missing a `requests`.";
				}
				return;
			}

			if (!payloadData["requests"].is_array()) {
				if (!session->IgnoreInvalidMessages()) {
					ret.closeCode = WebSocketCloseCode::InvalidDataFieldType;
					ret.closeReason = "Your `requests` is not an array.";
				}
				return;
			}

			RequestBatchExecutionType::RequestBatchExecutionType executionType = RequestBatchExecutionType::SerialRealtime;
			if (payloadData.contains("executionType") && !payloadData["executionType"].is_null()) {
				if (!payloadData["executionType"].is_number_unsigned()) {
					if (!session->IgnoreInvalidMessages()) {
						ret.closeCode = WebSocketCloseCode::InvalidDataFieldType;
						ret.closeReason = "Your `executionType` is not a number.";
					}
					return;
				}

				uint8_t requestedExecutionType = payloadData["executionType"];
				if (!RequestBatchExecutionType::IsValid(requestedExecutionType) || requestedExecutionType == RequestBatchExecutionType::None) {
					if (!session->IgnoreInvalidMessages()) {
						ret.closeCode = WebSocketCloseCode::InvalidDataFieldValue;
						ret.closeReason = "Your `executionType` has an invalid value.";
					}
				}

				// The thread pool must support 2 or more threads else parallel requests will deadlock.
				if (requestedExecutionType == RequestBatchExecutionType::Parallel && _threadPool.maxThreadCount() < 2) {
					if (!session->IgnoreInvalidMessages()) {
						ret.closeCode = WebSocketCloseCode::UnsupportedFeature;
						ret.closeReason = "Parallel request batch processing is not available on this system due to limited core count.";
					}
					return;
				}

				executionType = (RequestBatchExecutionType::RequestBatchExecutionType)requestedExecutionType;
			}

			if (payloadData.contains("variables") && !payloadData["variables"].is_null()) {
				if (!payloadData.is_object()) {
					if (!session->IgnoreInvalidMessages()) {
						ret.closeCode = WebSocketCloseCode::InvalidDataFieldType;
						ret.closeReason = "Your `variables` is not an object.";
					}
					return;
				}

				if (executionType == RequestBatchExecutionType::Parallel) {
					if (!session->IgnoreInvalidMessages()) {
						ret.closeCode = WebSocketCloseCode::UnsupportedFeature;
						ret.closeReason = "Variables are not supported in Parallel mode.";
					}
					return;
				}
			}

			bool haltOnFailure = false;
			if (payloadData.contains("haltOnFailure") && !payloadData["haltOnFailure"].is_null()) {
				if (!payloadData["haltOnFailure"].is_boolean()) {
					if (!session->IgnoreInvalidMessages()) {
						ret.closeCode = WebSocketCloseCode::InvalidDataFieldType;
						ret.closeReason = "Your `haltOnFailure` is not a boolean.";
					}
					return;
				}

				haltOnFailure = payloadData["haltOnFailure"];
			}

			std::vector<json> requests = payloadData["requests"];
			json variables = payloadData["variables"];
			std::vector<json> results;
			ProcessRequestBatch(session, executionType, requests, results, variables, haltOnFailure);

			ret.result["op"] = WebSocketOpCode::RequestBatchResponse;
			ret.result["d"]["requestId"] = payloadData["requestId"];
			ret.result["d"]["results"] = results;
			} return;
		default:
			if (!session->IgnoreInvalidMessages()) {
				ret.closeCode = WebSocketCloseCode::UnknownOpCode;
				ret.closeReason = std::string("Unknown OpCode: %s") + std::to_string(opCode);
			}
			return;
	}
}

// It isn't consistent to directly call the WebSocketServer from the events system, but it would also be dumb to make it unnecessarily complicated.
void WebSocketServer::BroadcastEvent(uint64_t requiredIntent, const std::string &eventType, const json &eventData, uint8_t rpcVersion)
{
	if (!_server.is_listening())
		return;

	_threadPool.start(Utils::Compat::CreateFunctionRunnable([=]() {
		// Populate message object
		json eventMessage;
		eventMessage["op"] = 5;
		eventMessage["d"]["eventType"] = eventType;
		eventMessage["d"]["eventIntent"] = requiredIntent;
		if (eventData.is_object())
			eventMessage["d"]["eventData"] = eventData;

		// Initialize objects. The broadcast process only dumps the data when its needed.
		std::string messageJson;
		std::string messageMsgPack;

		// Recurse connected sessions and send the event to suitable sessions.
		std::unique_lock<std::mutex> lock(_sessionMutex);
		for (auto & it : _sessions) {
			if (!it.second->IsIdentified()) {
				continue;
			}
			if (rpcVersion && it.second->RpcVersion() != rpcVersion) {
				continue;
			}
			if ((it.second->EventSubscriptions() & requiredIntent) != 0) {
				websocketpp::lib::error_code errorCode;
				switch (it.second->Encoding()) {
					case WebSocketEncoding::Json:
						if (messageJson.empty()) {
							messageJson = eventMessage.dump();
						}
						_server.send((websocketpp::connection_hdl)it.first, messageJson, websocketpp::frame::opcode::text, errorCode);
						it.second->IncrementOutgoingMessages();
						break;
					case WebSocketEncoding::MsgPack:
						if (messageMsgPack.empty()) {
							auto msgPackData = json::to_msgpack(eventMessage);
							messageMsgPack = std::string(msgPackData.begin(), msgPackData.end());
						}
						_server.send((websocketpp::connection_hdl)it.first, messageMsgPack, websocketpp::frame::opcode::binary, errorCode);
						it.second->IncrementOutgoingMessages();
						break;
				}
				if (errorCode)
					blog(LOG_ERROR, "[WebSocketServer::BroadcastEvent] Error sending event message: %s", errorCode.message().c_str());
			}
		}
		lock.unlock();
		if (IsDebugEnabled() && (EventSubscription::All & requiredIntent) != 0) // Don't log high volume events
			blog(LOG_INFO, "[WebSocketServer::BroadcastEvent] Outgoing event:\n%s", eventMessage.dump(2).c_str());
	}));
}