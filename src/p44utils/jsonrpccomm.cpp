//
//  jsonrpccomm.cpp
//  p44utils
//
//  Created by Lukas Zeller on 08.08.13.
//  Copyright (c) 2013 plan44.ch. All rights reserved.
//

#include "jsonrpccomm.hpp"


using namespace p44;


JsonRpcComm::JsonRpcComm(SyncIOMainLoop *aMainLoopP) :
  inherited(aMainLoopP),
  requestIdCounter(0)
{
  // set myself as handler of incoming JSON objects (which are supposed to be JSON-RPC 2.0
  setMessageHandler(boost::bind(&JsonRpcComm::gotJson, this, _2, _3));
}


JsonRpcComm::~JsonRpcComm()
{
}


void JsonRpcComm::setRequestHandler(JsonRpcRequestCB aJsonRpcRequestHandler)
{
  jsonRequestHandler = aJsonRpcRequestHandler;
}


static JsonObjectPtr jsonRPCObj()
{
  JsonObjectPtr obj = JsonObject::newObj();
  // the mandatory version string all objects need to have
  obj->add("jsonrpc", JsonObject::newString("2.0"));
  return obj;
}



#pragma mark - sending outgoing requests and responses


ErrorPtr JsonRpcComm::sendRequest(const char *aMethod, JsonObjectPtr aParams, JsonRpcResponseCB aResponseHandler)
{
  JsonObjectPtr request = jsonRPCObj();
  // the method or notification name
  request->add("method", JsonObject::newString(aMethod));
  // the optional parameters
  if (aParams) {
    request->add("params", aParams);
  }
  // in any case, count this call (even if it is a notification)
  requestIdCounter++;
  // in case this is a method call (i.e. a answer handler is specified), add the call ID
  if (aResponseHandler) {
    // add the ID so the callee can include it in the response
    request->add("id", JsonObject::newInt32(requestIdCounter));
    // remember it in our map
    pendingAnswers[requestIdCounter] = aResponseHandler;
  }
  // now send
  return sendMessage(request);
}


ErrorPtr JsonRpcComm::sendResult(const char *aJsonRpcId, JsonObjectPtr aResult)
{
  JsonObjectPtr response = jsonRPCObj();
  // add the result, can be NULL
  response->add("result", aResult);
  // add the ID so the caller can associate with a previous request
  response->add("id", JsonObject::newString(aJsonRpcId));
  // now send
  return sendMessage(response);
}


ErrorPtr JsonRpcComm::sendError(const char *aJsonRpcId, uint32_t aErrorCode, const char *aErrorMessage, JsonObjectPtr aErrorData)
{
  JsonObjectPtr response = jsonRPCObj();
  // create the error object
  JsonObjectPtr errorObj = JsonObject::newObj();
  errorObj->add("code", JsonObject::newInt32(aErrorCode));
  string errMsg;
  if (aErrorMessage) {
    errMsg = aErrorMessage;
  }
  else {
    errMsg = string_format("Error code %d (0x%X)", aErrorCode, aErrorCode);
  }
  errorObj->add("message", JsonObject::newString(errMsg));
  // add the data object if any
  if (aErrorData) {
    errorObj->add("data", aErrorData);
  }
  // add the error object
  response->add("error", errorObj);
  // add the ID so the caller can associate with a previous request
  response->add("id", JsonObject::newString(aJsonRpcId));
  // now send
  return sendMessage(response);
}


ErrorPtr JsonRpcComm::sendError(const char *aJsonRpcId, ErrorPtr aErrorToSend)
{
  if (!Error::isOK(aErrorToSend)) {
    return sendError(aJsonRpcId, (uint32_t)aErrorToSend->getErrorCode(), aErrorToSend->description().c_str());
  }
  return ErrorPtr();
}



#pragma mark - handling incoming requests and responses


void JsonRpcComm::gotJson(ErrorPtr aError, JsonObjectPtr aJsonObject)
{
  ErrorPtr respErr;
  JsonObjectPtr idObj;
  const char *idString = NULL;
  if (Error::isOK(aError)) {
    // received proper JSON, now check JSON-RPC specifics
    const char *method = NULL;
    JsonObjectPtr o = aJsonObject->get("jsonrpc");
    if (!o)
      respErr = ErrorPtr(new JsonRpcError(-32600, "Invalid Request - missing 'jsonrpc'"));
    else if (o->stringValue()!="2.0")
      respErr = ErrorPtr(new JsonRpcError(-32600, "Invalid Request - wrong version in 'jsonrpc'"));
    else {
      // get ID param (must be present for all messages except notification)
      idObj = aJsonObject->get("id");
      if (idObj) idString = aJsonObject->c_strValue();
      JsonObjectPtr paramsObj = aJsonObject->get("params");
      // JSON-RPC version is correct, check other params
      method = aJsonObject->getCString("method");
      if (method) {
        // this is a request (responses don't have the method member)
        if (*method==0)
          respErr = ErrorPtr(new JsonRpcError(-32600, "Invalid Request - empty 'method'"));
        else {
          // looks like a valid method or notification call
          if (!jsonRequestHandler) {
            // no handler -> method cannot be executed
            respErr = ErrorPtr(new JsonRpcError(-32601, "Method not found"));
          }
          else {
            // call handler to execute method or notification
            jsonRequestHandler(this, method, idString, paramsObj);
          }
        }
      }
      else {
        // this is a response (requests always have a method member)
        // - check if result or error
        JsonObjectPtr respObj = aJsonObject->get("result");
        if (!respObj) {
          // must be error, need further decoding
          respObj = aJsonObject->get("error");
          if (!respObj)
            respErr = ErrorPtr(new JsonRpcError(-32603, "Internal JSON-RPC error - response with neither 'result' nor 'error'"));
          else {
            // dissect error object
            ErrorCode errCode = -32603; // Internal RPC error
            const char *errMsg = "malformed Error response";
            // - try to get error code
            JsonObjectPtr o = respObj->get("code");
            if (o) errCode = o->int32Value();
            // - try to get error message
            o = respObj->get("message");
            if (o) errMsg = o->c_strValue();
            // compose error object from this
            respErr = ErrorPtr(new JsonRpcError(errCode, errMsg));
            // also get optional data element
            respObj = respObj->get("data");
          }
        }
        // Now we have either result or error.data in respObj, and respErr is Ok or contains the error code + message
        if (!idObj) {
          // errors without ID cannot be associated with calls made earlier, so just log the error
          LOG(LOG_WARNING,"JSON-RPC 2.0 error: Received response with no 'id' : %s\n", aJsonObject->c_strValue());
        }
        else {
          // dispatch by ID
          uint32_t requestId = idObj->int32Value();
          PendingAnswerMap::iterator pos = pendingAnswers.find(requestId);
          if (pos==pendingAnswers.end()) {
            // errors without ID cannot be associated with calls made earlier, so just log the error
            LOG(LOG_WARNING,"JSON-RPC 2.0 error: Received response with unknown 'id'=%d : %s\n", requestId, aJsonObject->c_strValue());
          }
          else {
            // found callback
            JsonRpcResponseCB cb = pos->second;
            pendingAnswers.erase(pos); // erase
            cb(this, respErr, respObj); // call
          }
        }
      }
    }
  }
  else {
    // no proper JSON received, create error response
    if (aError->isDomain(JsonCommError::domain())) {
      // some kind of parsing error
      respErr = ErrorPtr(new JsonRpcError(-32700, aError->description()));
    }
    else {
      // some other type of server error
      respErr = ErrorPtr(new JsonRpcError(-32000, aError->description()));
    }
  }
  // auto-generate error response for internally create errors
  if (!Error::isOK(respErr)) {
    sendError(idString, respErr);
  }
}












