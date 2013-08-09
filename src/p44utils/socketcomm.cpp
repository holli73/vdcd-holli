//
//  socketcomm.cpp
//  p44utils
//
//  Created by Lukas Zeller on 22.05.13.
//  Copyright (c) 2013 plan44.ch. All rights reserved.
//

#include "socketcomm.hpp"

#include <sys/ioctl.h>
#include <sys/poll.h>

using namespace p44;

SocketComm::SocketComm(SyncIOMainLoop *aMainLoopP) :
  FdComm(aMainLoopP),
  connectionOpen(false),
  isConnecting(false),
  serving(false),
  addressInfoList(NULL),
  currentAddressInfo(NULL),
  maxServerConnections(1),
  serverConnection(NULL),
  connectionFd(-1)
{
}


SocketComm::~SocketComm()
{
  internalCloseConnection();
}


void SocketComm::setConnectionParams(const char* aHostNameOrAddress, const char* aServiceOrPort, int aSocketType, int aProtocolFamily, int aProtocol)
{
  closeConnection();
  hostNameOrAddress = nonNullCStr(aHostNameOrAddress);
  serviceOrPortNo = nonNullCStr(aServiceOrPort);
  protocolFamily = aProtocolFamily;
  socketType = aSocketType;
  protocol = aProtocol;
}


#pragma mark - becoming a server

ErrorPtr SocketComm::startServer(ServerConnectionCB aServerConnectionHandler, int aMaxConnections)
{
  ErrorPtr err;

  struct servent *pse;
  struct sockaddr_in sin;
  int proto;
  int one = 1;
  int socketFD = -1;

  memset((char *) &sin, 0, sizeof(sin));
  if (protocolFamily==AF_INET) {
    sin.sin_family = (sa_family_t)protocolFamily;
    // set listening socket address
    if (nonLocal)
      sin.sin_addr.s_addr = htonl(INADDR_ANY);
    else
      sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    // get service / port
    if ((pse = getservbyname(serviceOrPortNo.c_str(), NULL)) != NULL)
      sin.sin_port = htons(ntohs((in_port_t)pse->s_port));
    else if ((sin.sin_port = htons((in_port_t)atoi(serviceOrPortNo.c_str()))) == 0) {
      err = ErrorPtr(new SocketCommError(SocketCommErrorCannotResolve,"Unknown service/port name"));
    }
    // protocol derived from socket type
    if (protocol==0) {
      // determine protocol automatically from socket type
      if (socketType==SOCK_STREAM)
        proto = IPPROTO_TCP;
      else
        proto = IPPROTO_UDP;
    }
    else
      proto = protocol;
  }
  else {
    // TODO: implement other portocol families, in particular AF_INET6
    err = ErrorPtr(new SocketCommError(SocketCommErrorUnsupported,"Unsupported protocol family"));
  }
  // now create and configure socket
  if (Error::isOK(err)) {
    socketFD = socket(PF_INET, socketType, proto);
    if (socketFD<0) {
      err = SysError::errNo("Cannot create server socket: ");
    }
    else {
      // socket created, set options
      if (setsockopt(socketFD,SOL_SOCKET,SO_REUSEADDR,(char *)&one,(int)sizeof(one)) == -1) {
        err = SysError::errNo("Cannot SETSOCKOPT SO_REUSEADDR: ");
      }
      else {
        // options ok, bind to address
        if (::bind(socketFD, (struct sockaddr *) &sin, (int)sizeof(sin)) < 0) {
          err = SysError::errNo("Cannot bind to port (server already running?): ");
        }
      }
    }
  }
  // listen
  if (Error::isOK(err)) {
    if (socketType==SOCK_STREAM && listen(socketFD, maxServerConnections) < 0) {
      err = SysError::errNo("Cannot listen on port: ");
    }
    else {
      // listen ok or not needed, make non-blocking
      makeNonBlocking(socketFD);
      // now socket is ready, register in mainloop to receive connections
      connectionFd = socketFD;
      serving = true;
      serverConnectionHandler = aServerConnectionHandler;
      // - install callback for when FD becomes writable (or errors out)
      mainLoopP->registerPollHandler(
        connectionFd,
        POLLIN,
        boost::bind(&SocketComm::connectionAcceptHandler, this, _1, _2, _3, _4)
      );
    }
  }

  return err;
}


bool SocketComm::connectionAcceptHandler(SyncIOMainLoop *aMainLoop, MLMicroSeconds aCycleStartTime, int aFd, int aPollFlags)
{
  ErrorPtr err;
  if (aPollFlags & POLLIN) {
    // server socket has data, means connection waiting to get accepted
    socklen_t cnamelen;
    struct sockaddr fsin;
    int clientFD = -1;
    cnamelen = sizeof(fsin);
    clientFD = accept(connectionFd, (struct sockaddr *) &fsin, &cnamelen);
    // TODO: report client's IP somehow
    if (clientFD>0) {
      // actually accepted
      // - call handler to create child connection
      SocketCommPtr clientComm;
      if (serverConnectionHandler) {
        clientComm = serverConnectionHandler(this);
      }
      if (clientComm) {
        // - remember
        clientConnections.push_back(clientComm);
        LOG(LOG_DEBUG, "New client connection accepted (now %d connections)\n", clientConnections.size());
        // - pass connection to child
        clientComm->passClientConnection(clientFD, this);
      }
      else {
        // can't handle connection, close immediately
        LOG(LOG_NOTICE, "connection not accepted - shut down\n");
        shutdown(clientFD, SHUT_RDWR);
        close(clientFD);
      }
    }
  }
  // handled
  return true;
}


void SocketComm::passClientConnection(int aFd, SocketComm *aServerConnectionP)
{
  // make non-blocking
  makeNonBlocking(aFd);
  // save and mark open
  serverConnection = aServerConnectionP;
  // set Fd and let FdComm base class install receive & transmit handlers
  setFd(aFd);
  isConnecting = false;
  connectionOpen = true;
  // call handler if defined
  if (connectionStatusHandler) {
    // connection ok
    connectionStatusHandler(this, ErrorPtr());
  }
}



void SocketComm::returnClientConnection(SocketComm *aClientConnectionP)
{
  // remove the client connection from the list
  for (SocketCommList::iterator pos = clientConnections.begin(); pos!=clientConnections.end(); ++pos) {
    if (pos->get()==aClientConnectionP) {
      // found, remove from list
      clientConnections.erase(pos);
      break;
    }
  }
  LOG(LOG_DEBUG, "Client connection terminated (now %d connections)\n", clientConnections.size());
}




#pragma mark - connecting to a client


bool SocketComm::connectable()
{
  return !hostNameOrAddress.empty();
}



ErrorPtr SocketComm::initiateConnection()
{
  int res;
  ErrorPtr err;

  if (!connectionOpen && !isConnecting && !serverConnection) {
    freeAddressInfo();
    if (hostNameOrAddress.empty()) {
      err = ErrorPtr(new SocketCommError(SocketCommErrorNoParams,"Missing connection parameters"));
      goto done;
    }
    // try to resolve host name
    struct addrinfo hint;
    memset(&hint, 0, sizeof(addrinfo));
    hint.ai_flags = 0; // no flags
    hint.ai_family = protocolFamily;
    hint.ai_socktype = socketType;
    hint.ai_protocol = protocol;
    res = getaddrinfo(hostNameOrAddress.c_str(), serviceOrPortNo.c_str(), &hint, &addressInfoList);
    if (res!=0) {
      // error
      err = ErrorPtr(new SocketCommError(SocketCommErrorCannotResolve, string_format("getaddrinfo error %d: %s", res, gai_strerror(res))));
      goto done;
    }
    // now try all addresses in the list
    // - init iterator pointer
    currentAddressInfo = addressInfoList;
    // - try connecting first address
    LOG(LOG_DEBUG, "Initiating connection to %s:%s\n", hostNameOrAddress.c_str(), serviceOrPortNo.c_str());
    err = connectNextAddress();
  }
done:
  if (!Error::isOK(err) && connectionStatusHandler) {
    connectionStatusHandler(this, err);
  }
  // return it
  return err;
}


void SocketComm::freeAddressInfo()
{
  if (!currentAddressInfo && addressInfoList) {
    // entire list consumed, free it
    freeaddrinfo(addressInfoList);
    addressInfoList = NULL;
  }
}


ErrorPtr SocketComm::connectNextAddress()
{
  int res;
  ErrorPtr err;

  // close possibly not fully open connection FD
  internalCloseConnection();
  // try to create a socket
  int socketFD = -1;
  // as long as we have more addresses to check and not already connecting
  bool connectingAgain = false;
  while (currentAddressInfo && !connectingAgain) {
    err.reset();
    socketFD = socket(currentAddressInfo->ai_family, currentAddressInfo->ai_socktype, currentAddressInfo->ai_protocol);
    if (socketFD==-1) {
      err = SysError::errNo("Cannot create client socket: ");
    }
    else {
      // usable address found, socket created
      // - make socket non-blocking
      makeNonBlocking(socketFD);
      // - initiate connection
      res = connect(socketFD, currentAddressInfo->ai_addr, currentAddressInfo->ai_addrlen);
      LOG(LOG_DEBUG, "- Attempting connection with address family = %d, protocol = %d\n", currentAddressInfo->ai_family, currentAddressInfo->ai_protocol);
      if (res==0 || errno==EINPROGRESS) {
        // connection initiated (or already open, but connectionMonitorHandler will take care in both cases)
        connectingAgain = true;
      }
      else {
        // immediate error connecting
        err = SysError::errNo("Cannot connect: ");
      }
    }
    // advance to next address
    currentAddressInfo = currentAddressInfo->ai_next;
  }
  if (!connectingAgain) {
    // exhausted addresses without starting to connect
    if (!err) err = ErrorPtr(new SocketCommError(SocketCommErrorNoConnection, "No connection could be established"));
    LOG(LOG_DEBUG, "Cannot initiate connection to %s:%s\n", hostNameOrAddress.c_str(), serviceOrPortNo.c_str(), err->description().c_str());
  }
  else {
    // connection in progress
    isConnecting = true;
    // - save FD
    connectionFd = socketFD;
    // - install callback for when FD becomes writable (or errors out)
    mainLoopP->registerPollHandler(
      connectionFd,
      POLLOUT,
      boost::bind(&SocketComm::connectionMonitorHandler, this, _1, _2, _3, _4)
    );
  }
  // clean up if list processed
  freeAddressInfo();
  // return status
  return err;
}



ErrorPtr SocketComm::socketError(int aSocketFd)
{
  ErrorPtr err;
  int result;
  socklen_t result_len = sizeof(result);
  if (getsockopt(aSocketFd, SOL_SOCKET, SO_ERROR, &result, &result_len) < 0) {
    // error, fail somehow, close socket
    err = SysError::errNo("Cant get socket error status: ");
  }
  else {
    err = SysError::err(result, "Socket Error status: ");
  }
  return err;
}



bool SocketComm::connectionMonitorHandler(SyncIOMainLoop *aMainLoop, MLMicroSeconds aCycleStartTime, int aFd, int aPollFlags)
{
  ErrorPtr err;
  if ((aPollFlags & POLLOUT) && isConnecting) {
    // became writable, check status
    err = socketError(aFd);
  }
  else if (aPollFlags & POLLHUP) {
    err = ErrorPtr(new SocketCommError(SocketCommErrorHungUp, "Connection HUP while opening (= connection rejected)"));
  }
  else if (aPollFlags & POLLERR) {
    err = socketError(aFd);
  }
  // now check if successful
  if (Error::isOK(err)) {
    // successfully connected
    connectionOpen = true;
    isConnecting = false;
    currentAddressInfo = NULL; // no more addresses to check
    freeAddressInfo();
    LOG(LOG_DEBUG, "Connection to %s:%s established\n", hostNameOrAddress.c_str(), serviceOrPortNo.c_str());
    // call handler if defined
    if (connectionStatusHandler) {
      // connection ok
      connectionStatusHandler(this, ErrorPtr());
    }
    // let FdComm base class operate open connection (will install handlers)
    setFd(aFd);
  }
  else {
    // this attempt has failed, try next (if any)
    LOG(LOG_DEBUG, "- Connection attempt failed: %s\n", err->description().c_str());
    // this will return no error if we have another address to try
    err = connectNextAddress();
    if (err) {
      // no next attempt started, report error
      LOG(LOG_WARNING, "Connection to %s:%s failed: %s\n", hostNameOrAddress.c_str(), serviceOrPortNo.c_str(), err->description().c_str());
      internalCloseConnection();
      if (connectionStatusHandler) {
        connectionStatusHandler(this, err);
      }
      freeAddressInfo();
    }
  }
  // handled
  return true;
}


void SocketComm::setConnectionStatusHandler(SocketCommCB aConnectedHandler)
{
  // set handler
  connectionStatusHandler = aConnectedHandler;
}



void SocketComm::closeConnection()
{
  if (connectionOpen) {
    // close the connection
    internalCloseConnection();
    // report to handler
    if (connectionStatusHandler) {
      // connection ok
      ErrorPtr err = ErrorPtr(new SocketCommError(SocketCommErrorClosed, "Connection closed"));
      connectionStatusHandler(this, err);
      LOG(LOG_NOTICE, "Connection to %s:%s explicitly closed\n", hostNameOrAddress.c_str(), serviceOrPortNo.c_str());
    }
  }
}


void SocketComm::internalCloseConnection()
{
  // unregister from main loop
  if (serving) {
    // serving socket
    // - close listening socket
    mainLoopP->unregisterPollHandler(connectionFd);
    close(connectionFd);
    connectionFd = -1;
    serving = false;
    // - close all child connections
    for (SocketCommList::iterator pos = clientConnections.begin(); pos!=clientConnections.end(); ++pos) {
      (*pos)->closeConnection();
    }
  }
  else if (connectionOpen || isConnecting) {
    // stop monitoring data connection
    setFd(-1);
    // to make sure, also unregister handler for connectionFd (in case FdComm had no fd set yet)
    mainLoopP->unregisterPollHandler(connectionFd);
    close(connectionFd);
    connectionFd = -1;
    connectionOpen = false;
    isConnecting = false;
    // if this was a client connection to our server, let server know
    if (serverConnection) {
      serverConnection->returnClientConnection(this);
      serverConnection = NULL;
    }
  }
}


bool SocketComm::connected()
{
  return connectionOpen;
}


bool SocketComm::connecting()
{
  return isConnecting;
}


#pragma mark - handling data exception


void SocketComm::dataExceptionHandler(int aFd, int aPollFlags)
{
  DBGLOG(LOG_DEBUG, "FdComm::dataExceptionHandler(fd==%d, pollflags==0x%X)\n", aFd, aPollFlags);
  if (aPollFlags & POLLHUP) {
    // other end has closed connection
    // - close my end
    internalCloseConnection();
    if (connectionStatusHandler) {
      // report reason for closing
      connectionStatusHandler(this, ErrorPtr(new SocketCommError(SocketCommErrorHungUp,"Connection closed (HUP)")));
    }
  }
  else if (aPollFlags & POLLIN) {
    // Note: on linux a socket closed server side does not return POLLHUP, but POLLIN with no data
    // alerted for read, but nothing to read any more: assume connection closed
    ErrorPtr err = socketError(aFd);
    if (Error::isOK(err))
      err = ErrorPtr(new SocketCommError(SocketCommErrorHungUp,"Connection alerts POLLIN but has no more data (intepreted as HUP)"));
    LOG(LOG_WARNING, "Connection to %s:%s reported POLLIN but no data; error: %s\n", hostNameOrAddress.c_str(), serviceOrPortNo.c_str(), err->description().c_str());
    // - shut down
    internalCloseConnection();
    if (connectionStatusHandler) {
      // report reason for closing
      connectionStatusHandler(this, err);
    }
  }
  else if (aPollFlags & POLLERR) {
    // error
    ErrorPtr err = socketError(aFd);
    LOG(LOG_WARNING, "Connection to %s:%s reported error: %s\n", hostNameOrAddress.c_str(), serviceOrPortNo.c_str(), err->description().c_str());
    // - shut down
    internalCloseConnection();
    if (connectionStatusHandler) {
      // report reason for closing
      connectionStatusHandler(this, err);
    }
  }
}
