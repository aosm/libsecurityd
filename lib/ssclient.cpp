/*
 * Copyright (c) 2000-2004 Apple Computer, Inc. All Rights Reserved.
 * 
 * @APPLE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */


//
// ssclient - SecurityServer client interface library
//
#include "sstransit.h"
#include <servers/netname.h>
#include <security_utilities/debugging.h>

using MachPlusPlus::check;
using MachPlusPlus::Bootstrap;


namespace Security {
namespace SecurityServer {


//
// The process-global object
//
UnixPlusPlus::StaticForkMonitor ClientSession::mHasForked;
ModuleNexus<ClientSession::Global> ClientSession::mGlobal;
bool ClientSession::mSetupSession;
const char *ClientSession::mContactName;

//
// Construct a client session
//
ClientSession::ClientSession(Allocator &std, Allocator &rtn)
: ClientCommon(std, rtn), mCallback(NULL), mCallbackContext(NULL)
{ }


//
// Destroy a session
//
ClientSession::~ClientSession()
{ }


void
ClientSession::registerForAclEdits(DidChangeKeyAclCallback *callback, void *context)
{
	mCallback = callback;
	mCallbackContext = context;
}

//
// Activate a client session: This connects to the SecurityServer and executes
// application authentication
//
void ClientSession::activate()
{
	// Guard against fork-without-exec. If we are the child of a fork
	// (that has not exec'ed), our apparent connection to SecurityServer
	// is just a mirage, and we better reset it.
	if (mHasForked()) {
		secdebug("SSclnt", "process has forked (now pid=%d) - resetting connection object", getpid());
		mGlobal.reset();
	}
		
	// now pick up the (new or existing) connection state
	Global &global = mGlobal();
    Thread &thread = global.thread();
    if (!thread) {
		// first time for this thread - use abbreviated registration
		IPCN(ucsp_client_setupThread(UCSP_ARGS, mach_task_self()));
        thread.registered = true;
        secdebug("SSclnt", "Thread registered with %s", mContactName);
	}
}


//
// The contactName method allows the caller to explicitly override the bootstrap
// name under which SecurityServer is located. Use this only with great caution,
// and probably only for debugging.
// Note that no explicit locking is done here. It is the caller's responsibility
// to make sure this is called from thread-safe context before the real dance begins.
//
void ClientSession::contactName(const char *name)
{
	mContactName = name;
}

const char *ClientSession::contactName() const
{
	return mContactName;
}


//
// Construct the process-global state object.
// The ModuleNexus construction magic will ensure that this happens uniquely
// even if the face of multithreaded attack.
// Do note that the mSetupSession (session creation) case is gated by a global flag,
// and it's the caller's responsibility not to multithread-race it.
//
ClientSession::Global::Global()
{
    // find server port
	serverPort = findSecurityd();
    
    // send identification/setup message
    string extForm;
    try {
        myself = OSXCode::main();
        extForm = myself->encode();
        secdebug("SSclnt", "my OSXCode extForm=%s", extForm.c_str());
    } catch (...) {
        // leave extForm empty
        secdebug("SSclnt", "failed to obtain my own OSXCode");
    }

	ClientSetupInfo info = { 0x1234, SSPROTOVERSION };
	
    // cannot use UCSP_ARGS here because it uses mGlobal() -> deadlock
    Thread &thread = this->thread();
	
	if (mSetupSession) {
		secdebug("SSclnt", "sending session setup request");
		mSetupSession = false;	// reset global
		IPCN(ucsp_client_setupNew(serverPort, thread.replyPort, &rcode,
			mach_task_self(), info, extForm.c_str(), &serverPort.port()));
		secdebug("SSclnt", "new session server port is %d", serverPort.port());
	} else {
		IPCN(ucsp_client_setup(serverPort, thread.replyPort, &rcode,
			mach_task_self(), info, extForm.c_str()));
	}
    thread.registered = true;	// as a side-effect of setup call above
	IFDEBUG(serverPort.requestNotify(thread.replyPort));
	secdebug("SSclnt", "contact with %s established", mContactName);
}


//
// Reset the connection.
// This discards all client state accumulated for the securityd link.
// Existing connections will go stale and fail; new connections will
// re-establish the link. This is an expert tool ONLY. If you don't know
// exactly how this gig is danced, you don't want to call this. Really.
//
void ClientSession::reset()
{
	secdebug("SSclnt", "resetting client state (OUCH)");
	mGlobal.reset();
}


//
// Common utility for finding the registered securityd port for the current
// session. This does not cache the port anywhere, though it does effectively
// cache the name.
//
Port ClientSession::findSecurityd()
{
	if (!mContactName)
	{
		mContactName = getenv(SECURITYSERVER_BOOTSTRAP_ENV);
		if (!mContactName)
			mContactName = SECURITYSERVER_BOOTSTRAP_NAME;
	}

    secdebug("SSclnt", "Locating %s", mContactName);
    Port serverPort = Bootstrap().lookup(mContactName);
	secdebug("SSclnt", "contacting %s at port %d (version %d)",
		mContactName, serverPort.port(), SSPROTOVERSION);
	return serverPort;
}


//
// Subsidiary process management.
// This does not go through the generic securityd-client setup.
//
void ClientSession::childCheckIn(Port serverPort, Port taskPort)
{
	check(ucsp_client_childCheckIn(findSecurityd(), serverPort, taskPort));
}


//
// Temporary hack - deal with securityd-generated special error codes by
// calling a special callback for ACL editing. This facility will move entirely
// into securityd at some point (hopefully near), after which this call will
// disappear with a contented sigh.
//
void ClientSession::addApplicationAclSubject(KeyHandle key, CSSM_ACL_AUTHORIZATION_TAG tag)
{
	/* Notify our client if they are interested. */
	if (mCallback)
	{
		secdebug("keyacl", "ClientSession::addApplicationAclSubject(keyHandle: %lu tag: %lu)", key, tag);
		mCallback(mCallbackContext, *this, key, tag);
	}
	else
		secdebug("keyacl", "ClientSession::addApplicationAclSubject() with NULL mCallback");
}


} // end namespace SecurityServer
} // end namespace Security