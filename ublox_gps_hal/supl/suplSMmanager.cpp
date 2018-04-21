/*******************************************************************************
 *
 * Copyright (C) u-blox AG 
 * u-blox AG, Thalwil, Switzerland
 *
 * All rights reserved.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose without fee is hereby granted, provided that this entire notice
 * is included in all copies of any software which is or includes a copy
 * or modification of this software and in all copies of the supporting
 * documentation for such software.
 *
 * THIS SOFTWARE IS BEING PROVIDED "AS IS", WITHOUT ANY EXPRESS OR IMPLIED
 * WARRANTY. IN PARTICULAR, NEITHER THE AUTHOR NOR U-BLOX MAKES ANY
 * REPRESENTATION OR WARRANTY OF ANY KIND CONCERNING THE MERCHANTABILITY
 * OF THIS SOFTWARE OR ITS FITNESS FOR ANY PARTICULAR PURPOSE.
 *
 *******************************************************************************
 *
 * Project: PE_ANS
 *
 ******************************************************************************/
/*!
  \file
  \brief  SET state Machine adminstrator

  Managing and allocation the state machines for SUPL SET
*/
/*******************************************************************************
 * $Id: suplSMmanager.cpp 64704 2013-01-10 16:39:29Z jon.bowern $
 * $HeadURL: http://svn.u-blox.ch/GPS/SOFTWARE/hmw/android/release/release_2.0/supl/suplSMmanager.cpp $
 ******************************************************************************/
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

#include <time.h>
#include <malloc.h>
#include <openssl/hmac.h>
#include "openssl/ssl.h"
#include <openssl/err.h>
#include <semaphore.h>

#include "upldecod.h"
#include "uplsend.h"

#include "suplSMmanager.h"
#include "std_types.h"

#include "ubx_log.h"
#include "ubx_timer.h"
#include "ubx_localDb.h"
#include "ubx_agpsIf.h"
#include "ubx_niIf.h"
#include "ubxgpsstate.h"
#include "gps_thread.h"

///////////////////////////////////////////////////////////////////////////////
// Types & Definitions

//! states of the SUPL SM
typedef enum 
{
    START,      //!< empty start state, after SM alvocated
    NO_NETWORK, //!< Waiting for network to become available
    AUTH,       //!< Waiting for authorization from UI
    WAIT_RES,   //!< Wait for sesponce after a SUPLSTART sent
    RRLP,       //!< RRLP session state
    RRLP_RESP,  //!< RRLP response session state
    END         //!< empty end state, after the SM can be deallocated
} SMState_t;

#define MAX_SM_INSTANCES 5      //!< Maximum number of SUPL stame machine instances
								// If this is changed to > 1, then the position publish to framework
								// control mechanism needs to be made smarter to allow for multiple 
								// supl sessions.

#define START_SID_NUMBER 1001   				//!< Start Session Id number
#define MAX_SID_NUMBER (START_SID_NUMBER + 200)	//!< Max SID number

#define MAX_UPL_PACKET 5000     //!< Dimension of the max UPL packet coming from the network

#define SUPL_STD_TIMEOUT  10					//!< the default timeout, for the SUPL specs, is 10 seconds
#define SUPL_NOTIFICATION_TIMEOUT 120			//!< Timeout default for the notification from UI, 2 minutes
#define SUPL_POLL_INTERVAL	(3600 * 24 * 1000)	//!< 1 day - what should this really be? TODO
#define SUPL_NONETWORK_TIMEOUT 60				//!< 60 seconds timeout if Supl transaction requested but no network

//! Handler for the state machine instancies */
typedef struct suplHandler {
    int sid;                    			//!< Unique session identifier ==  SET ID
    SlpSessionID_t *pSlpId;      			//!< must be linked with the SLP ID
    SetSessionID_t *pSetId;      			//!< must be linked with the SLP ID
    SMState_t state;            			//!< state of the current SM instance
    BIO *bio;                   			//!< Bio handler
    SSL_CTX *ctx;               			//!< SSL context handler
    long long hash;             			//!< Hash of the SUPL INIT
    NotificationType_t notificationType;    //!< Notification type of SUPL INIT
	EncodingType_t encodingType;			//!< Notification texts encoding type
	OCTET_STRING_t* pRequestorId;			//!< Pointer to received requestor id text. Used for notify/verify request to framework
	FormatIndicator_t requestorIdType;		//!< Received requestor id text type. Used for notify/verify request to framework
	OCTET_STRING_t* pClientName;			//!< Pointer to received client name text. Used for notify/verify request to framework
	FormatIndicator_t clientNameType;		//!< Received requestor client name type. Used for notify/verify request to framework
    time_t timeout;             			//!< socket file descriptor of the current session
    int rrlpRefNum;             			//!< rrlp reference number for delayed responce
    int  nonProxy;              			//!< The server is requesting a non proxy mode
    int reqHorAccuracy;         			//!< Requested Horizontal accuracy
    int reqVerAccuracy;         			//!< Requested Vertical accuracy
    struct suplHandler *pNext;  			//!< Pointer to the next handler active in the linked list
	RrlpRespData_t responseType;			//!< Type of pending response
	int msaPosResponseTime;					//!< The time to wait before responsing to server with MSA data
	int QopDelay;							//!< Delay trieved from Quality of Position 'delay' field in SUPLINIT
	PosMethod_t requestedPosMethod;			//!< The position method to be used thoughout the transaction
	bool networkInitiated;					//!< true if the session is an NI one, false if SI
	bool assistanceRequested;				//!< true is assitance data was requested from the server, false if not
	struct ULP_PDU* pNiMsg;					//!< Pointer NI supl init message
} suplHandler_t;

///////////////////////////////////////////////////////////////////////////////
// Static data
static const char *stateInfo[END+1] = {			//!< Logging texts for Supl states
    "START",
    "NO_NETWORK",
    "AUTH",
    "WAIT_RES",
    "RRLP",
    "RRLP_RESP",
    "END",
};

static const char* _strPosMethodType[] = {		//!< Logging texts for position method
	"agpsSETassisted",
	"agpsSETbased",
	"agpsSETassistedpref",
	"agpsSETbasedpref",
	"autonomousGPS",
	"aFLT",
	"eCID",
	"eOTD",
	"oTDOA",
	"noPosition",
};

static const char* _strUlpMessageType[] = {		//!< Logging texts for Supl message types
	"NOTHING",	
	"msSUPLINIT",
	"msSUPLSTART",
	"msSUPLRESPONSE",
	"msSUPLPOSINIT",
	"msSUPLPOS",
	"msSUPLEND",
	"PR_msSUPLAUTHREQ",
	"msSUPLAUTHRESP",
};

static suplHandler_t *s_pQueueTail						= NULL;		//!< Tail of the supl sessions list
static int64_t s_lastSuplSiTime							= 0;		//!< Last time a Supl SI session was performed
static GpsControlEventInterface* s_pGpsControlInterface	= NULL;		//!< Interface to Gps engine control
static void* s_pEventContext 							= NULL;		//!< Pointer to context for Gps engine control interface calls
static pthread_mutex_t s_handlerMutex;								//!< Handler list mutex

///////////////////////////////////////////////////////////////////////////////
// Local functions
static int suplSM(suplHandler_t *pHandler, struct ULP_PDU *pMsg, SuplCmd_t cmd);
static suplHandler_t *allocateSM(bool ni);
static bool suplInsertHandler(suplHandler_t *pHandler, const SetSessionID_t *pSetId);
static suplHandler_t *searchSMInst(int sid, bool lock);
static void endSuplSession(suplHandler_t* pHandler);
static void freeSuplHandler(suplHandler_t *pHandler);
static int getNewSid(void);
static void processRawSuplMessage(const char *buffer, int size, suplHandler_t *pReceivedHandler);
static long long calculateHash(const char *buffer, int size);
static int connectTimeout(BIO *bio, int sid, int seconds);
static int createUplSession(suplHandler_t *pHandler);
static void generateSuplEndMsg(suplHandler_t * pHandler, StatusCode statusCode);
static int sendPositionResponse(suplHandler_t *pHandler, char* pSendBuffer, int size);
static void setNotificationAndResponse(NotificationType_t ans1cNotifyType, 
	GpsNiNotifyFlags* pNotifyFlags, 
	GpsUserResponseType* pDefaultResponse);
static GpsNiEncodingType convertAsn1Encoding(EncodingType_t encodingType);
static void copyAndConvertText(char* pDest, int maxLen, const OCTET_STRING_t* pSuplString);
static bool isSuplPossible(void);
static void determineAssistance(suplHandler_t* pHandler, suplPosInitParam_t* pPosInitParams);
static int startSiSession(suplHandler_t* pHandler);
static void startNiSession(suplHandler_t *pHandler);
static bool compareOctets(const OCTET_STRING_t *pOct1, const OCTET_STRING_t *pOct2);
static bool verifySlpSessionId(SlpSessionID_t *pSlpSessionId_1, SlpSessionID_t *pSlpSessionId_2);
static bool verifySetSessionId(const SetSessionID_t *pSetSessionId_1, const SetSessionID_t *pSetSessionId_2);
static void safeFreeHandler(suplHandler_t* pHandler);


///////////////////////////////////////////////////////////////////////////////
//! Initialise the Supl state machine
/*! 

*/
void suplInit(void)
{
	pthread_mutex_init(&s_handlerMutex, NULL);
}

///////////////////////////////////////////////////////////////////////////////
//!  De-initialise the Supl sate machine
/*! 

*/
void suplDeinit(void)
{
	pthread_mutex_destroy(&s_handlerMutex);
}

///////////////////////////////////////////////////////////////////////////////
//! Register the event interface 
/*! Function to register with the Supl session manager the Gps engine control
    event interface.
  \param pEventInterface 	: Pointer to the evenp interface
  \param pContext			: Pointer to the context to use in all calls to event the event interface
*/
void suplRegisterEventCallbacks(GpsControlEventInterface *pEventInterface, void* pContext)
{
	s_pGpsControlInterface = pEventInterface;
	s_pEventContext = pContext;
}

///////////////////////////////////////////////////////////////////////////////
//! Function checking if there are any active Supl sessions
/*! Function checking if there are any active Supl sessions
  \return      : true is there are sessions active, false if not
*/
bool suplActiveSessions(void)
{
	assert(pthread_self() == g_gpsDrvMainThread);
	return s_pQueueTail != NULL;
}

///////////////////////////////////////////////////////////////////////////////
//! Counts the number of SI or NI sessions
/*! 
  \param ni	: true if NI sessions to be counted, false if SI sessions to be counted
  \return   : the number of NI or SI sessions active
*/
int suplCountSessions(bool ni) 
{
	int count = 0;
	pthread_mutex_lock(&s_handlerMutex);
	suplHandler_t *pHandler = s_pQueueTail;
	while(pHandler != NULL)
	{
		if ((pHandler->networkInitiated && ni) ||
            (!pHandler->networkInitiated && !ni))
		{
			count++;
		}
		pHandler = pHandler->pNext;
	}
	pthread_mutex_unlock(&s_handlerMutex);
	return count;
}

///////////////////////////////////////////////////////////////////////////////
//! Function checking if the SET needs to initiate a SUPL transaction
/*! this function is used to be called regularly to check and if necessary
    initiate a SUPL transaction from the SET
*/
void suplCheckForSiAction(void)
{
	assert(pthread_self() == g_gpsDrvMainThread);
	
	if (CGpsIf::getInstance()->getMode() != GPS_POSITION_MODE_STANDALONE)
	{
		// Need to check for possible start of a SUPL transaction
		int64_t timeNowMs = getMonotonicMsCounter();
		//LOGV("%s: %lli %lli", __FUNCTION__, timeNowMs, s_lastSuplSiTime);
		
		if (timeNowMs > s_lastSuplSiTime)
		{
			LOGV("%s: SUPL SI", __FUNCTION__);
			suplStartSetInitiatedAction();
			
			s_lastSuplSiTime = timeNowMs + SUPL_POLL_INTERVAL;
		}
	}
}

///////////////////////////////////////////////////////////////////////////////
//! Function for handling pending SUPL transaction
/*! This function is has to be called regularly for checking timeouts or fulfilments of
    pending SUPL transactions
*/
void suplCheckPendingActions(void)
{
	assert(pthread_self() == g_gpsDrvMainThread);

	pthread_mutex_lock(&s_handlerMutex);
    suplHandler_t *pHandler =  s_pQueueTail;  
	pthread_mutex_unlock(&s_handlerMutex);
	
	CMyDatabase* pDatabase = CMyDatabase::getInstance();
	
	assert(pDatabase != NULL);
	
    /* browse the list of handler */
    while (pHandler != NULL)
    {
        /* save next in the chain */
		pthread_mutex_lock(&s_handlerMutex);
        suplHandler_t *pNext = pHandler->pNext;
		pthread_mutex_unlock(&s_handlerMutex);
			
        /* check for timeout of existing SUPL transactions */
		time_t now  = time(NULL);
		
		if ((pHandler->state == START) && (pHandler->networkInitiated))
		{
			// Get the NI session going
			if (suplSM(pHandler, pHandler->pNiMsg, SUPL_NO_CMD) < 0)
			{
				endSuplSession(pHandler);	// error/supl session ended, deallocate handler
			}
		}
        else if ((pHandler->timeout != -1) && (pHandler->timeout <= now))
        {
			// Normal timeout
			LOGV("%s: SUPL Timer expired",__FUNCTION__);
			/* Clear the timer */
			pHandler->timeout = -1;

			/* send the message to the state machine */
			suplSM(pHandler, NULL, SUPL_TIMEOUT);

			/* After timeout,  session always ended, deallocate handler */
			endSuplSession(pHandler);
        }
		else if (pHandler->state == RRLP_RESP)
		{
			if (pHandler->responseType == RESP_POSITION_DATA)
			{
				/* check if we can fulfil any pending SUPL transactions requiring position information */
				double lat, lon = 0.0, speed;
				int accuracy = pHandler->reqHorAccuracy;
				bool posAvail = (pDatabase->getData(CMyDatabase::DATA_LATITUDE_DEGREES, lat) && 
								 pDatabase->getData(CMyDatabase::DATA_LONGITUDE_DEGREES, lon));
				bool speedAvail = pDatabase->getData(CMyDatabase::DATA_SPEED_KNOTS, speed);
				
				pDatabase->getData(CMyDatabase::DATA_ERROR_RADIUS_METERS, accuracy);
				/*LOGV("%s: Pos response pending - PA %i  SA %i  Acc %i  RHAcc %i  RVAcc %i", 
					__FUNCTION__, posAvail, speedAvail, accuracy, pHandler->reqHorAccuracy, pHandler->reqVerAccuracy);
				*/
				if ((posAvail) &&
					(speedAvail) &&
					(accuracy < pHandler->reqHorAccuracy) &&
					(pHandler->reqVerAccuracy == -1 || accuracy < pHandler->reqVerAccuracy))
				{
					LOGV("%s: Position available with accuracy %d, lat %f, lon %f, speed %f", 
						 __FUNCTION__, accuracy, lat, lon, speed);

					/* send the message to the state machine */
					if (suplSM(pHandler, NULL, SUPL_POS_AVAIL) < 0)
					{
						/* error/supl session ended, deallocate handler */
						endSuplSession(pHandler);
					}
				}
			}
			else if (pHandler->responseType == RESP_MSA_DATA)
			{
				/*LOGV("%s: MSA response still pending - delay %ld - TO %ld",
					 __FUNCTION__, 
					 pHandler->msaPosResponseTime - now,
					 pHandler->timeout - now);
				*/
				if (pHandler->msaPosResponseTime < now)
				{
					LOGV("%s: MSA request timeout - send what data we have", __FUNCTION__);
					if (suplSM(pHandler, NULL, SUPL_MSA_DATA_AVAIL) < 0)
					{
						/* error/supl session ended, deallocate handler */
						endSuplSession(pHandler);
					}
				}
			}
			else
			{
				// Nothing ready
			}
        }
		else if (pHandler->state == NO_NETWORK)
		{
			// Session pending
			if (CRilIf::getInstance()->isConnected())
			{
				if (suplSM(pHandler, NULL, SUPL_NETWORK_CONNECTED) < 0)
				{
					/* error/supl session ended, deallocate handler */
					endSuplSession(pHandler);
				}
			}
		}

        /* go to the next */
        pHandler = pNext;
    }
}

///////////////////////////////////////////////////////////////////////////////
//! Add Supl sockets to listen on
/*! Function used to add the sockets of any existing SUPL transactions to the list
    of sockets 'select' will listen on
  \param pRfds  : pointer to file descriptor array (used by 'select') to added SUPL sockets to
  \param pMaxFd : pointer allowing the largest file descriptor in the array to be returned
  \return       : 1 if queue empty, 0 if not
*/
int suplAddUplListeners(fd_set *pRfds, int *pMaxFd)
{
	assert(pthread_self() == g_gpsDrvMainThread);
	assert(pRfds);
	assert(pMaxFd);

    /* check if the handler queue is empty! */
    if (s_pQueueTail == NULL)
    {
        return 1;
    }    
	
	suplHandler_t *pHandler = s_pQueueTail;  

    /* browse the list of handler */
    while (pHandler != NULL)
    {
        /* Make the FD_SET */
        if (pHandler->bio != 0)
        {
            int fd = BIO_get_fd(pHandler->bio, NULL);
//			LOGV("%s: Open SUPL handle - Listerning for data on %d", __FUNCTION__, fd);
            FD_SET(fd, pRfds);
			
			if ((fd + 1) > *pMaxFd)
			{
				*pMaxFd = fd + 1;
			}
        }
		pthread_mutex_lock(&s_handlerMutex);
        pHandler = pHandler->pNext;
		pthread_mutex_unlock(&s_handlerMutex); 
	}

    return 0;
}

///////////////////////////////////////////////////////////////////////////////
//! Function  for reading a SUPL socket
/*! This function must be used to read any input on a SUPL socket
  \param pRfds : Pointer to file descriptor array
  \return      : 1 if session queue is empty, 0 if not
*/
int suplReadUplSock(fd_set *pRfds)
{
	assert(pthread_self() == g_gpsDrvMainThread);
	assert(pRfds);
	
    /* check if the handler queue is empty! */
    if (s_pQueueTail == NULL)
    {
        return 1;
    } 

    suplHandler_t *pHandler = s_pQueueTail;  
    suplHandler_t *pNext = NULL;  
    char buffer[MAX_UPL_PACKET];
    int res;
	
	/* browse the list of handler */
    while (pHandler != NULL)
    {
        int fd = BIO_get_fd(pHandler->bio, NULL);
        /* Store the next pointer */
		pthread_mutex_lock(&s_handlerMutex);
        pNext = pHandler->pNext;
		pthread_mutex_unlock(&s_handlerMutex);
		
        if (pHandler->bio != NULL)
        {
            if (FD_ISSET(fd, pRfds))
            {
                LOGV("%s: Received data over UPL socket %d", __FUNCTION__, fd);
                /* read into a local buffer */
                res = BIO_read(pHandler->bio, buffer, MAX_UPL_PACKET);
                LOGV("%s: read result is %d", __FUNCTION__, res);
                if (res > MAX_UPL_PACKET)
                {
                    LOGE("%s: Unexpected reading size!", __FUNCTION__);
                }
                else if (res > 0)
                {
                    processRawSuplMessage(buffer, res, pHandler);
                }
                else
                {
                    /* Connection has been closed! deallocate the state machine... */
					endSuplSession(pHandler);
                }
            }
        }
        pHandler = pNext;
    }

    return 0;
//lint -e{818} remove Pointer parameter 'pRfds' (line 445) could be declared as pointing to const
}

///////////////////////////////////////////////////////////////////////////////
//! Function to start (initiate) a SUPL transaction
/*! Function to start (initiate) a SUPL transaction
*/
bool suplStartSetInitiatedAction (void)
{
	assert(pthread_self() == g_gpsDrvMainThread);
	
	CRilIf::getInstance()->requestCellInfo();
	if (!isSuplPossible())
	{
		LOGV("%s: SI failed - Supl not possible", __FUNCTION__);
		return false;
	}
	
	/* get new instance of SET Initiated SM */
	suplHandler_t *pHandler = allocateSM(false);
	if (pHandler == NULL)
	{
		LOGE("%s: no more instance set available!!! error", __FUNCTION__);
		return false;
	}
	
	bool success = true;
	if (suplInsertHandler(pHandler, NULL))
	{
		logAgps.write(0x00000000, "%d # a-gps session starts", pHandler->sid);
		logAgps.write(0x00000005, "%d # u-blox, 1.0.0, # version number", pHandler->sid);
		
		if (suplSM(pHandler, NULL, SUPL_ASK_FOR_AGPS) < 0)
		{
			/* error/supl session ended, deallocate handler */
			endSuplSession(pHandler);
			success = false;
		}
	}
	else
	{
		// Could not insert into handler list - Free basic handler structure
        safeFreeHandler(pHandler);
		success = false;
	}
	
	return success;
}

///////////////////////////////////////////////////////////////////////////////
//! Network initiated message received (usually from SMS)
/*! If the network has initiated a SUPL transaction (a SUPLINIT message is received),
    this function must be called
  \param buffer: Pointer to buffer containing Supl message
  \param size  : Size of the buffer
*/
void suplHandleNetworkInitiatedAction(const char *buffer, int size)
{
	assert(buffer);
	
	// directly digest the gsm message
	// Make sure we have up to date cell info
	CRilIf::getInstance()->requestCellInfo();
	if (isSuplPossible())
	{
		processRawSuplMessage(buffer, size, NULL);
	}
	else
	{
		LOGV("%s: NI failed - Supl not possible", __FUNCTION__);
	}
}

///////////////////////////////////////////////////////////////////////////////
//! Handle injection of authorisation into an existing session
/*! 
  \param sid : session ID
  \param cmd : Session command to execute
*/
void suplHandleAuthorization(int sid, int cmd)
{
	if (pthread_self() != g_gpsDrvMainThread)
    {
        LOGE("%s: Not in the main Thread (%.8X %.8X)", __FUNCTION__, 
             (unsigned int) pthread_self(), 
             (unsigned int) g_gpsDrvMainThread);
    }
	suplHandler_t *pHandler = searchSMInst(sid, true);		// Need to use lock
	
	if (pHandler)
	{
		if (suplSM(pHandler, NULL, (SuplCmd_t) cmd) < 0)
		{
			/* error/supl session ended - deallocate handler */
			endSuplSession(pHandler);
		}
	}
	else
	{
		LOGE("%s: No existing session with sid %i", __FUNCTION__, sid);
	}
}

///////////////////////////////////////////////////////////////////////////////
// Static functions

//! Process a buffer containing a Supl message
/*!
  \param buffer: Pointer to payload buffer containing SUPL message
  \param size  : Size of the buffer
*/
static void processRawSuplMessage(const char *buffer, int size, suplHandler_t *pReceivedHandler)
{
	assert(buffer);
	
    suplHandler_t *pHandler = NULL;

	/* The decode the incoming message */
	struct ULP_PDU *pMsg = uplDecode(buffer, size);
	
	if (pMsg == NULL)
	{
		// Decode failed/no message decoded
		LOGV("%s: Msg not decoded", __FUNCTION__);
		return;
	}
	LOGV("%s: Msg decoded. Msg type %i(%s)", 
		 __FUNCTION__, pMsg->message.present, _LOOKUPSTR(pMsg->message.present, UlpMessageType));

	if (pMsg->message.present == UlpMessage_PR_msSUPLINIT)
	{
		assert(pthread_self() != g_gpsDrvMainThread);		// Should be on an arbitary thread
		LOGV("%s: SLP (NI) initiated session...", __FUNCTION__);
		if (suplCountSessions(true) > 0)
		{
			// There are existing NI sessions - reject this request
			LOGV("%s: Existing NI session. Rejecting request", __FUNCTION__);
		}
		else
		{
			// Need to prepare handler structure & then insert in to handler list
			// After insertion, main thread can change handler's contents
			pHandler = allocateSM(true);
			if (pHandler == NULL)
			{
				LOGV("%s: no more instance slp available!!! error", __FUNCTION__);
                LOGV("%s: Deallocate messege %p", __FUNCTION__, pMsg);
				ASN_STRUCT_FREE(asn_DEF_ULP_PDU, pMsg);			/* Deallocate the message decoded */
			}
			else
			{
				// Handler structure allocated - fill in some more
				/* calcolate the hash here... */
				pHandler->hash = calculateHash(buffer, size);
                LOGV("%s:%d NiMsg saved here %p in thread %u", __FUNCTION__, __LINE__, pMsg, (unsigned int) pthread_self());
				pHandler->pNiMsg = pMsg;						// Save until NI session is started

				if (suplInsertHandler(pHandler, pMsg->sessionID.setSessionID))
				{
					// Handler contents inserted into handler list and can now be changed by main thread
					// This also means the Supl session has begun from our point of view
					logAgps.write(0x00000000, "%d # a-gps session starts", pHandler->sid);
					logAgps.write(0x00000005, "%d # u-blox, 1.0.0, # version number", pHandler->sid);

					if (s_pGpsControlInterface)
					{
						s_pGpsControlInterface->requestStart_cb(s_pEventContext);
					}
				}
				else
				{
					// Could not insert into handler list
                    LOGV("%s:%d Deallocate messege %p", __FUNCTION__, __LINE__, pMsg);
					ASN_STRUCT_FREE(asn_DEF_ULP_PDU, pMsg);			// Deallocate the decoded message
                    safeFreeHandler(pHandler);
				}
			}
		}
	}
	else
	{
		// Not the SLPINIT - extracting the session Id
        if (pthread_self() != g_gpsDrvMainThread)
        {
            /* unexpected, not from the main thread */
            LOGW("%s: Wrong thread", __FUNCTION__);
        }
        else
        {
            int sid = extractSid(pMsg);
            LOGV("%s: sid %d", __FUNCTION__, sid);
            pHandler = searchSMInst(sid, true);

            if (pHandler == NULL)
            {
                if (pReceivedHandler != NULL)
                {
                    /* Received something over established connection but no associated handler */
                    if (suplSM(pReceivedHandler, pMsg, SUPL_NO_CMD) < 0)
                    {
                        /* error/supl session ended, deallocate handler */
                        endSuplSession(pReceivedHandler);
                    }
                }
                else
                {
                    /* No instance associated!!! */
                    LOGV("%s: No session with sid %d ", __FUNCTION__, sid);
                }
            }
            else
            {
                if (suplSM(pHandler, pMsg, SUPL_NO_CMD) < 0)
                {
                    /* error/supl session ended, deallocate handler */
                    endSuplSession(pHandler);
                }
            }
            /* Deallocate the message decoded */
            LOGV("%s: Deallocate messege %p", __FUNCTION__, pMsg);
            ASN_STRUCT_FREE(asn_DEF_ULP_PDU, pMsg);
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
//! Process the state machine for a supl session
/*!
  \param pHandler : Pointer to state machine structure for a supl session
  \param pMsg     : Pointer to a structure conatining the decoded Supl message
  \param cmd      : Cmd to the state machine
  \return 0 session is continuing, < 0 session is finished
*/
static int suplSM(suplHandler_t *pHandler, struct ULP_PDU *pMsg, SuplCmd_t cmd)
{
	assert(pHandler);
	assert(pthread_self() == g_gpsDrvMainThread);
    int res = 0;	// Success by default

    LOGV("%s: SM entry with Id = %d in state %s (%d) with command %d", __FUNCTION__, pHandler->sid,stateInfo[pHandler->state],pHandler->state,cmd);

    /* State selector */
    switch (pHandler->state)
    {
    case START:
		LOGV("%s: Handling START", __FUNCTION__);
        
        if (pMsg == NULL)
        {
			// No incoming msg, so expecting a command from device
            if (cmd == SUPL_ASK_FOR_AGPS)
            {
				// This is Set Initiation (SI)
				logAgps.write(0x10000000, "%d # network connecting...", pHandler->sid);
				if (CRilIf::getInstance()->isConnected())
				{
					res = startSiSession(pHandler);
				}
				else
				{
					// Supl session is pending
					LOGV("%s: No network. Deferring SI until network present", __FUNCTION__);
					pHandler->state = NO_NETWORK;
					pHandler->timeout = time(NULL) + SUPL_NONETWORK_TIMEOUT;
				}
            }
            else
            {
                LOGV("%s: Unexpected command %d", __FUNCTION__, cmd);
                pHandler->state = END;
                res = -1;
            }
        }
        else 
        {
            /* Message coming from Network (NI hopefully) */
            if (pMsg->message.present == UlpMessage_PR_msSUPLINIT)
            {
				LOGV("%s: Received SUPLINIT <====", __FUNCTION__);
				logAgps.write(0x00000002, "%d, 1.0.0, SUPL_INIT # SUPL_MODE : NI %s", pHandler->sid, 
								((pMsg->message.choice.msSUPLINIT.posMethod == PosMethod_agpsSETbased) ||
								 (pMsg->message.choice.msSUPLINIT.posMethod == PosMethod_agpsSETbasedpref)) 	? "MSB" :
								((pMsg->message.choice.msSUPLINIT.posMethod == PosMethod_agpsSETassisted) ||
								 (pMsg->message.choice.msSUPLINIT.posMethod == PosMethod_agpsSETassistedpref)) 	? "MSA" : 
																												  "? ? ?");
				// This is a Network Initiation (NI)
                LOGV("%s: method requested is %ld", __FUNCTION__, pMsg->message.choice.msSUPLINIT.posMethod);
                /* check the positioning method */
                if (pMsg->message.choice.msSUPLINIT.posMethod != PosMethod_agpsSETbased &&
                    pMsg->message.choice.msSUPLINIT.posMethod != PosMethod_agpsSETassistedpref &&
                    pMsg->message.choice.msSUPLINIT.posMethod != PosMethod_agpsSETbasedpref &&
					pMsg->message.choice.msSUPLINIT.posMethod != PosMethod_agpsSETassisted &&
                    pMsg->message.choice.msSUPLINIT.posMethod != PosMethod_eCID &&
                    pMsg->message.choice.msSUPLINIT.posMethod != PosMethod_noPosition)
                {
                    LOGV("%s: The position method is not supported!", __FUNCTION__);
					// As we haven't yet opened a socket to the server, we have no communications
					// channel, so do nothing. Network will timeout.
					pHandler->state = END;
					res = -1;
					break;
                }

                /* check if non proxy mode! */
                if (pMsg->message.choice.msSUPLINIT.sLPMode == SLPMode_nonProxy)
                {
                    LOGV("%s: Only Proxy mode is supported!", __FUNCTION__);
					// As we haven't yet opened a socket to the server, we have no communications
					// channel, so do nothing. Network will timeout.
					pHandler->state = END;
					res = -1;
					break;
                }

				// Notification
				pHandler->notificationType = NotificationType_noNotificationNoVerification;
				pHandler->encodingType = -1;
				pHandler->pRequestorId = NULL;
				pHandler->requestorIdType = -1;
				pHandler->pClientName = NULL;
				pHandler->clientNameType = -1;
				
                /* Notification type */
                if (pMsg->message.choice.msSUPLINIT.notification != NULL)
                {
                    LOGV("%s: notification type %ld", __FUNCTION__, pMsg->message.choice.msSUPLINIT.notification->notificationType);
                    pHandler->notificationType = pMsg->message.choice.msSUPLINIT.notification->notificationType;
					
					if (pMsg->message.choice.msSUPLINIT.notification->encodingType != NULL)
					{
						pHandler->encodingType = *pMsg->message.choice.msSUPLINIT.notification->encodingType;
					}
					pHandler->pRequestorId = pMsg->message.choice.msSUPLINIT.notification->requestorId;
					if (pMsg->message.choice.msSUPLINIT.notification->requestorIdType != NULL)
					{
						pHandler->requestorIdType = *pMsg->message.choice.msSUPLINIT.notification->requestorIdType;
					}
					
					pHandler->pClientName = pMsg->message.choice.msSUPLINIT.notification->clientName;
					if (pMsg->message.choice.msSUPLINIT.notification->clientNameType != NULL)
					{
						pHandler->clientNameType = *pMsg->message.choice.msSUPLINIT.notification->clientNameType;
					}
                }

				/* default no QOP requested */
				pHandler->reqVerAccuracy = -1;
				pHandler->reqHorAccuracy = -1;

                /* check if there is QOP field */
                if (pMsg->message.choice.msSUPLINIT.qoP != NULL)
                {
                    /* QOP present */
					if (pMsg->message.choice.msSUPLINIT.qoP->delay != NULL)
					{
						pHandler->QopDelay = 1 << *pMsg->message.choice.msSUPLINIT.qoP->delay;
						LOGV("%s: Qop delay %d", __FUNCTION__, pHandler->QopDelay);
					}
                    else
                    {
                        /* No delay */
                        LOGV("%s: No Qop delay in SUPLINIT", __FUNCTION__);
                    }

                    pHandler->reqHorAccuracy = (int) (10 * (pow(1.1, (double) pMsg->message.choice.msSUPLINIT.qoP->horacc) - 1));
                    LOGV("%s: Horizontal QOP requested %d", __FUNCTION__, pHandler->reqHorAccuracy);
                    if (pMsg->message.choice.msSUPLINIT.qoP->veracc != NULL)
                    {
                        /* vertical accuracy is present */
                        pHandler->reqVerAccuracy = *pMsg->message.choice.msSUPLINIT.qoP->veracc;
						LOGV("%s: Verical QOP requested %d", __FUNCTION__, pHandler->reqVerAccuracy);
                    }
                    else
                    {
                        /* No vertical accuracy */
                        LOGV("%s: No vertical accuracy requested in SUPLINIT", __FUNCTION__);
                    }
					/*		OPTIONAL
					long	*veracc;
					long	*maxLocAge;
					long	*delay;
					*/
					logAgps.write(0x00000002, "%d, %d, %d, %d, %d # QoP", pHandler->sid, 
								pHandler->reqHorAccuracy, pHandler->reqVerAccuracy, 
								0,1,1); // todo fix this
                }
                else
                {
					// No H or V accuracies
                    LOGV("%s: No QOP requested in SUPLINIT", __FUNCTION__);
                }
				
                if (pMsg->message.choice.msSUPLINIT.sLPMode == SLPMode_nonProxy)
                {
                    /* Not possible, will be given a not possible! - Deferred error reporting */
                    pHandler->nonProxy = 1;
                }
				// Copy position method
				pHandler->requestedPosMethod = pMsg->message.choice.msSUPLINIT.posMethod;
				
                /* copy slpSessionId*/
                pHandler->pSlpId = copySlpId(pMsg->sessionID.slpSessionID);

                // Timeout is set to UI notifcation timeout, which is handled else where
                pHandler->timeout = -1;
				
				logAgps.write(0x10000000, "%d # network connecting...", pHandler->sid);
				if (CRilIf::getInstance()->isConnected())
				{
                    /* check if setid not set (must be empty) */
                    if (pMsg->sessionID.setSessionID != NULL || pMsg->sessionID.slpSessionID == NULL)
                    {
                        /* Create a UPL session */
                        if (createUplSession(pHandler) == -1)
                        {
                            LOGV("%s: Cannot create the session", __FUNCTION__);
                        }
                        else
                        {
                            LOGV("%s: error in session ID", __FUNCTION__);
                            generateSuplEndMsg(pHandler, StatusCode_invalidSessionId);
                        }
                        pHandler->state = END;
                        res = -1;
                        break;
                    }
                    else
                    {
                        if (pMsg->message.choice.msSUPLINIT.posMethod == PosMethod_noPosition)
                        {
                            /* Create a UPL session */
                            if (createUplSession(pHandler) == -1)
                            {
                                LOGV("%s: Cannot create the session", __FUNCTION__);
                            }
                            else
                            {
                                generateSuplEndMsg(pHandler, StatusCode_unspecified);
                            }
                            pHandler->state = END;
                            res = -1;
                            break;
                        }
                        else
                        {
                    
                            // Ask for notification from the UI 
                            startNiSession(pHandler);			// Can recursively call suplSM
					
                            // Do not do any state handling here
                            // Also pHandler could be a floating pointer if an error occurred in recursed suplSM called
                        }
                    }
				}
				else
				{
					// Supl session is pending
					LOGV("%s: No network. Deferring NI until network present", __FUNCTION__);
					pHandler->state = NO_NETWORK;
					pHandler->timeout = time(NULL) + SUPL_NONETWORK_TIMEOUT;
				}
            }
            else 
            {
                LOGV("%s: Unexpected message from network", __FUNCTION__);
                asn_fprint(stdout,&asn_DEF_ULP_PDU, pMsg);
				// As we haven't yet opened a socket to the server, we have no communications
				// channel, so do nothing. Network will timeout.  TODO - Or do we send a defered error?
                pHandler->state = END;
                res = -1;
            }
        }
        break;
		
	case NO_NETWORK:
		LOGV("%s: Handling NO_NETWORK", __FUNCTION__);
		if (cmd == SUPL_NETWORK_CONNECTED)
		{
			if (pHandler->networkInitiated)
			{
                if (pHandler->pNiMsg->sessionID.setSessionID != NULL || pHandler->pNiMsg->sessionID.slpSessionID == NULL)
                {
                    /* Create a UPL session */
                    if (createUplSession(pHandler) == -1)
                    {
                        LOGV("%s: Cannot create the session", __FUNCTION__);
                    }
                    else
                    {
                        LOGV("%s: error in session ID", __FUNCTION__);
                        generateSuplEndMsg(pHandler, StatusCode_invalidSessionId);
                    }
                    pHandler->state = END;
                    res = -1;
                    break;
                }
                else
                {
                    if (pHandler->pNiMsg->message.choice.msSUPLINIT.posMethod == PosMethod_noPosition)
                    {
                        /* Create a UPL session */
                        if (createUplSession(pHandler) == -1)
                        {
                            LOGV("%s: Cannot create the session", __FUNCTION__);
                        }
                        else
                        {
                            generateSuplEndMsg(pHandler, StatusCode_unspecified);
                        }
                        pHandler->state = END;
                        res = -1;
                        break;
                    }
                    else
                    {
                        // Ask for notification from the UI 
                        startNiSession(pHandler);			// Can recursively call suplSM
				
                        // Do not do any state handling here
                        // Also pHandler could be a floating pointer if an error occurred in recursed suplSM called
                    }
                }
			}
			else
			{
				res = startSiSession(pHandler);
			}
		}
		else if (cmd == SUPL_TIMEOUT)
		{
			// Timed out waiting for network connection
			logAgps.write(0x22000001, "%d # timeout", pHandler->sid);
			res = -1;
		}
		break;
		
    case WAIT_RES:
        LOGV("%s: Handling WAIT_RES with message %p", __FUNCTION__, pMsg);
        if (pMsg == NULL)
        {
            // No incoming msg, so expecting a command from device
            if (cmd == SUPL_TIMEOUT)
            {
				// Timed out waiting for server response
				LOGV("%s: Sending SUPLEND (Timeout) ====>", __FUNCTION__);
				generateSuplEndMsg(pHandler, StatusCode_unspecified);
				logAgps.write(0x22000001, "%d, # timeout", pHandler->sid);
                res = -1;
            }
        }
        else
        {
            /* check incoming message from network */
            if (pMsg->message.present == UlpMessage_PR_msSUPLEND)
            {
				LOGV("%s: Received SUPLEND <====", __FUNCTION__);
                /* Next state: END */
                if (pMsg->message.choice.msSUPLEND.statusCode != NULL)
                {
                    LOGV("%s: Reason is :%ld", __FUNCTION__, *(pMsg->message.choice.msSUPLEND.statusCode) );
					logAgps.write(0x22000000, "%d, %d, SUPL_END", pHandler->sid, pMsg->message.choice.msSUPLEND.statusCode);
                }
				else
					logAgps.write(0x02000000, "%d, 1.0.0, SUPL_END", pHandler->sid);
				pHandler->state = END;
				res = -1;				// Always needed here as Supl session ended and resources will need to be freed.
            }
            else if (pMsg->message.present != UlpMessage_PR_msSUPLRESPONSE)
            {
                /* Unexpected message - End session*/
                LOGV("%s: Unexpected answer - Not SUPLRESPONSE", __FUNCTION__);
				LOGV("%s: Sending SUPLEND ====>", __FUNCTION__);
				generateSuplEndMsg(pHandler, StatusCode_unexpectedMessage);
                res = -1;
            }
			else
			{
				PosMethod_t	posMethod = pMsg->message.choice.msSUPLRESPONSE.posMethod;
				LOGV("%s: Received SUPLRESPONSE <====", __FUNCTION__);
				logAgps.write(0x02000000, "%d, 1.0.0, SUPL_RESPONSE # SUPL_MODE : SI %s", pHandler->sid, 
							(posMethod == PosMethod_agpsSETbased)    ? "MSB" : 
							(posMethod == PosMethod_agpsSETassisted) ? "MSA" : "? ? ?" );
    
				// Handle SUPLRESPONSE msg
				if (pMsg->message.choice.msSUPLRESPONSE.sLPAddress != NULL)
				{
					/* Non proxy mode unsupported... error */
					LOGV("%s: Can not handle because non proxy mode expected but not supported (%i)", 
						 __FUNCTION__, pMsg->message.choice.msSUPLRESPONSE.sLPAddress->present);
					LOGV("%s: Sending SUPLEND ====>", __FUNCTION__);
					generateSuplEndMsg(pHandler, StatusCode_nonProxyModeNotSupported);
					res = -1;
				}
                else if (!verifySetSessionId(pMsg->sessionID.setSessionID, pHandler->pSetId))
                {
                    /* Inconsistency of session ID */
					LOGV("%s: wrong sid %d inseat of %d", __FUNCTION__, pHandler->sid, (int) pMsg->sessionID.setSessionID->sessionId);
					LOGV("%s: Sending SUPLEND ====>", __FUNCTION__);
                    ASN_STRUCT_FREE(asn_DEF_SetSessionID, pHandler->pSetId);
                    pHandler->pSetId = copySetId(pMsg->sessionID.setSessionID);
                    pHandler->pSlpId = copySlpId(pMsg->sessionID.slpSessionID);
					generateSuplEndMsg(pHandler, StatusCode_invalidSessionId);
					res = -1;
                }
				else
				{
					LOGV("%s: Processing SUPLRESPONSE - Pos Method %li(%s)", 
						 __FUNCTION__, posMethod, _LOOKUPSTR((U) posMethod, PosMethodType));
					
					if ((posMethod == PosMethod_agpsSETbased) ||
						(posMethod == PosMethod_agpsSETassisted))
					{
						LOGV("%s: Handling SUPLRESPONSE (%s)", 
							 __FUNCTION__, _LOOKUPSTR((U) posMethod, PosMethodType));

						// Handle SUPL_RESPONSE & SetBased
						suplPosInitParam_t par;

						/* copy the slpId from the receiving message */
						pHandler->pSlpId = copySlpId(pMsg->sessionID.slpSessionID);

						par.pSetId = pHandler->pSetId;
						par.pSlpId = pHandler->pSlpId;

						determineAssistance(pHandler, &par);
						
						/* In that state we are asking for the assistance data, most probably we
						   do not have the position */
						par.verEn = 0;
//						par.hash = 0x1122334455667788LL;
						
						// Pass through requested position method
						par.requestedPosMethod = posMethod;
						
						/* Ok, this is what we expect... */
						LOGV("%s: Sending SUPLINITPOS ====>", __FUNCTION__);
						sendSuplPosInit(pHandler->bio, &par);
						/* Next state */
						pHandler->state = RRLP;
						
						/* Set the timeout */
						pHandler->timeout = time(NULL) + SUPL_STD_TIMEOUT;
					}
					else
					{
						/* Only SET based or Assisted possible here... */
						LOGV("%s: Can not handle. Not MS-Based or MS-Assist", __FUNCTION__);
						LOGV("%s: Sending SUPLEND ====>", __FUNCTION__);
						generateSuplEndMsg(pHandler, StatusCode_posMethodMismatch);
						res = -1;
					}
				}
			}
        }
        break;
		
    case AUTH:
		LOGV("%s: Handling AUTH", __FUNCTION__);
        if (cmd == SUPL_AUTH_GRANT)
        {
            /* Authorization granted!!! */
            LOGV("%s: Authorization granted", __FUNCTION__);
			
            /* Create a UPL session */
            if (createUplSession(pHandler) == -1)
            {
                LOGV("%s: Cannot create the session", __FUNCTION__);
                pHandler->state = END;
                res = -1;
            }
            else
            {
                if (pHandler->nonProxy)
                {
                    LOGV("%s: Proxy mode not supported", __FUNCTION__);
                    /* we must send the SUPL_END, and then close the connection */
					LOGV("%s: Sending SUPLEND ====>", __FUNCTION__);
					generateSuplEndMsg(pHandler, StatusCode_proxyModeNotSupported);
                    /* Next state: END */
                    pHandler->state = END;
                    res = -1;
                }
                else
                {
                    suplPosInitParam_t par;

                    par.pSetId = pHandler->pSetId;
                    par.pSlpId = pHandler->pSlpId;

                    /* here need to be verified what can be done depending on the GPS state */
					double lat, lon, speed;
					int accuracy = pHandler->reqHorAccuracy;
					
					CMyDatabase* pDatabase = CMyDatabase::getInstance();
					pDatabase->getData(CMyDatabase::DATA_ERROR_RADIUS_METERS, accuracy);
					
					if ((pDatabase->getData(CMyDatabase::DATA_LATITUDE_DEGREES, lat) && pDatabase->getData(CMyDatabase::DATA_LONGITUDE_DEGREES, lon)) &&	
						pDatabase->getData(CMyDatabase::DATA_SPEED_KNOTS, speed) &&
                        (pHandler->reqHorAccuracy == -1 || accuracy < pHandler->reqHorAccuracy) &&
                        (pHandler->reqVerAccuracy == -1 || accuracy < pHandler->reqVerAccuracy) )
                    {
						// Don't need assistance as we already have position
                        par.posEn = 1;
                        par.assEn = 0;
                    }
                    else
                    {
						// Don't have position, request assistance
                        par.posEn = 0;
                        par.assEn = 1;
						pHandler->assistanceRequested = true;
                    }

                    /* The hash is needed */
                    par.verEn = 1;
                    par.hash = pHandler->hash;
					par.requestedPosMethod = pHandler->requestedPosMethod;
					
                    /* we must send the SUPL_POSINIT */
					LOGV("%s: Sending SUPLPOSINIT ====>", __FUNCTION__);
                    sendSuplPosInit(pHandler->bio, &par);

                    /* Set the timeout */
                    pHandler->timeout = time(NULL) + SUPL_STD_TIMEOUT;
                
                    /* Next state:  RRLP*/
                    pHandler->state = RRLP;
                }
            }
        }
        else if (cmd == SUPL_AUTH_DENIED)
        {
            /* Authorization denied!!! */
            LOGV("%s: Authorization denied", __FUNCTION__);
            /* Create a UPL session */
            if (createUplSession(pHandler) == -1)
            {
                LOGV("%s: Cannot create the session", __FUNCTION__);
                pHandler->state = END;
                res = -1;
            }
            else
            {
                if (pHandler->notificationType == NotificationType_notificationOnly)
                {
                    LOGV("%s: Impossible! notification only, UI must always return GRANT", __FUNCTION__);
                }

                LOGV("%s: Authorization denied...", __FUNCTION__);
				LOGV("%s: Sending SUPLEND ====>", __FUNCTION__);
				generateSuplEndMsg(pHandler, StatusCode_consentDeniedByUser);
                /* Next state: END */
                pHandler->state = END;
                res = -1;
            }
        }
        break;
		
    case RRLP:
		LOGV("%s: Handling RRLP", __FUNCTION__);
        if (pMsg == NULL)
        {
			LOGV("%s: Local cmd %d", __FUNCTION__, cmd);
            /* Only timeout available */
            if (cmd == SUPL_TIMEOUT)
            {
				LOGV("%s: Sending SUPLEND (Timeout) ====>", __FUNCTION__);
				generateSuplEndMsg(pHandler, StatusCode_unspecified);
				logAgps.write(0x22000001, "%d, # timeout", pHandler->sid);
                res = -1;
            }
        }
        else
        {
			LOGV("%s: Msg from network: Present %i(%s)",
				 __FUNCTION__, pMsg->message.present, _LOOKUPSTR(pMsg->message.present, UlpMessageType));
			/* check incoming message */
			if (pMsg->message.present == UlpMessage_PR_msSUPLPOS)
			{
				LOGV("%s: Received SUPLPOS <====", __FUNCTION__);
				suplPosParam_t par;
				aux_t aux;

				logAgps.write(0x02000000, "%d, 1.0.0, SUPL_POS # recv", pHandler->sid);
    
				if (pMsg->message.choice.msSUPLPOS.posPayLoad.present == PosPayLoad_PR_rrlpPayload)
				{
					par.buffer = rrlpProcessing(pHandler->sid, pMsg->message.choice.msSUPLPOS.posPayLoad.choice.rrlpPayload.buf,
												pMsg->message.choice.msSUPLPOS.posPayLoad.choice.rrlpPayload.size,
												&(par.size),
												&aux);
                    LOGI("%s: rrlp data prepared",__FUNCTION__);
												
/* Do not report an error if assistance data is not received */
/*
					if ((pHandler->networkInitiated) && (pHandler->assistanceRequested) && (!aux.assistDataReceived))
					{
						// Terminate NI because no assist data received
						LOGV("%s: Assistance data not received", __FUNCTION__);
						LOGV("%s: Sending SUPLEND ====>", __FUNCTION__);
						generateSuplEndMsg(pHandler, StatusCode_dataMissing);
						res = -1;
						pHandler->state = END;
					}
					else 
*/
                    if ((pHandler->networkInitiated) && (pHandler->pNiMsg) && (pHandler->pNiMsg->sessionID.setSessionID) &&
                        ((pHandler->pNiMsg->sessionID.setSessionID->sessionId != pMsg->sessionID.setSessionID->sessionId) ||
                         (memcmp(&pHandler->pNiMsg->sessionID.setSessionID->setId, &pMsg->sessionID.setSessionID->setId, sizeof(SETId_t)) != 0 )))
                    {
                        LOGV("%s: Invalid session id received, supl END sent", __FUNCTION__);
                        generateSuplEndMsg(pHandler, StatusCode_invalidSessionId);
                        res = -1;
                    }
                    else if (pHandler->pNiMsg && pHandler->pNiMsg->sessionID.slpSessionID == NULL)
                    {
                        LOGV("%s: Invalid slp session id, supl END sent", __FUNCTION__);
                        generateSuplEndMsg(pHandler, StatusCode_invalidSessionId);
                        res = -1;
                    }
                    else if ((pHandler->networkInitiated) && (pHandler->pNiMsg) && 
                             !verifySlpSessionId(pHandler->pNiMsg->sessionID.slpSessionID, pMsg->sessionID.slpSessionID))
                    {
                        ASN_STRUCT_FREE(asn_DEF_SlpSessionID, pHandler->pSlpId);
                        pHandler->pSlpId = copySlpId(pMsg->sessionID.slpSessionID);
                        LOGV("%s:%d Invalid slp session id, supl END sent", __FUNCTION__, __LINE__);
                        generateSuplEndMsg(pHandler, StatusCode_invalidSessionId);
                        res = -1;
                    }
                    else if ((!pHandler->networkInitiated) && 
                             !verifySlpSessionId(pHandler->pSlpId, pMsg->sessionID.slpSessionID))
                    {
                        ASN_STRUCT_FREE(asn_DEF_SlpSessionID, pHandler->pSlpId);
                        pHandler->pSlpId = copySlpId(pMsg->sessionID.slpSessionID);
                        LOGV("%s:%d Invalid slp session id, supl END sent", __FUNCTION__, __LINE__);
                        generateSuplEndMsg(pHandler, StatusCode_invalidSessionId);
                        res = -1;
                    }
                    else if (!verifySetSessionId(pMsg->sessionID.setSessionID, pHandler->pSetId))
                    {
                        /* Inconsistency of session ID */
                        LOGV("%s: wrong sid %d inseat of %d", __FUNCTION__, pHandler->sid, (int) pMsg->sessionID.setSessionID->sessionId);
                        LOGV("%s: Sending SUPLEND ====>", __FUNCTION__);
                        ASN_STRUCT_FREE(asn_DEF_SetSessionID, pHandler->pSetId);
                        pHandler->pSetId = copySetId(pMsg->sessionID.setSessionID);
                        generateSuplEndMsg(pHandler, StatusCode_invalidSessionId);
                        res = -1;
                    }
                    else if (par.buffer == NULL)
					{
						// No immediate response - so check response time
						if (aux.responseTime <= 0)
						{
							// Wrong RRLP packet - so end session
							LOGV("%s: Wrong RRLP packet %d", __FUNCTION__, cmd);
							LOGV("%s: Sending SUPLEND ====>", __FUNCTION__);
							generateSuplEndMsg(pHandler, StatusCode_protocolError);
							res = -1;
						}
						else
						{
							/* Now I have a timeout for answering with a position response RRLP message */
							time_t now = time(NULL);
							int maxDelay = CUbxGpsState::getInstance()->getNiResponseTimeout();
							
							if (aux.responseType == RESP_MSA_DATA)
							{
								// MSA
								pHandler->timeout = -1;		// There is no timeout as such
								pHandler->msaPosResponseTime = maxDelay < aux.responseTime ? maxDelay : aux.responseTime;
								pHandler->msaPosResponseTime += now;
							}
							else
							{
								// MSB
								if (pHandler->QopDelay > 0)
								{
									pHandler->timeout = pHandler->QopDelay;
								}
								else
								{
									pHandler->timeout = aux.responseTime;
								}
								
								// Config setting gives upper limit on delay
								if (pHandler->timeout > maxDelay)
								{
									pHandler->timeout = maxDelay;
								}
								pHandler->timeout += now;
							}
							
							pHandler->state = RRLP_RESP;
							pHandler->rrlpRefNum = aux.referenceNumber;
							pHandler->reqHorAccuracy = aux.reqAccuracy;
							pHandler->reqVerAccuracy = -1;
							pHandler->responseType = aux.responseType;
							
							logAgps.write(0x00000002, "%d, %d, %d, %d, %d, %d # QoP", pHandler->sid, 
										pHandler->reqHorAccuracy, pHandler->reqVerAccuracy, 
										0, 0, pHandler->msaPosResponseTime); // todo 
							LOGV("%s: Need to response to SUPL request in %is", __FUNCTION__, aux.responseTime);
						}
					}
					else
					{
						// Send a position response to server
						par.pSlpId = pHandler->pSlpId;
						par.pSetId = pHandler->pSetId;
				
						/* Ok, this is what we expect... */
						LOGI("%s: Sending SUPLPOS ====>", __FUNCTION__);
						logAgps.write(0x00000002, "%d, # assist ack", pHandler->sid);
						sendSuplPos(pHandler->bio, &par);

						MC_FREE(par.buffer);
				
						/* Set the timeout */
						pHandler->timeout = time(NULL) + SUPL_STD_TIMEOUT;
					}
				}
				else
				{
					// Unexpected SUPLPOS payload
					LOGV("%s: Not an RRLP payload", __FUNCTION__);
					LOGV("%s: Sending SUPLEND ====>", __FUNCTION__);
					generateSuplEndMsg(pHandler, StatusCode_posProtocolMismatch);
					res = -1;
					pHandler->state = END;
				}
			}
			else if (pMsg->message.present == UlpMessage_PR_msSUPLEND)
			{
				LOGV("%s: Received SUPLEND <====", __FUNCTION__);
                if (pMsg->message.choice.msSUPLEND.statusCode != NULL)
                {
					logAgps.write(0x22000000, "%d, %d, SUPL_END", pHandler->sid, pMsg->message.choice.msSUPLEND.statusCode);
				    LOGV("%s: Reason is :%ld", __FUNCTION__, *(pMsg->message.choice.msSUPLEND.statusCode) );
                }
				else
					logAgps.write(0x02000000, "%d, 1.0.0, SUPL_END", pHandler->sid);
				
				if (pMsg->message.choice.msSUPLEND.position != NULL)
				{
					// Position estimate received, send to receiver
					long latSign = pMsg->message.choice.msSUPLEND.position->positionEstimate.latitudeSign;
					double lat = pMsg->message.choice.msSUPLEND.position->positionEstimate.latitude ;
					if (latSign)
					{
						lat *= -1;
					}
					double lng = pMsg->message.choice.msSUPLEND.position->positionEstimate.longitude;
					lat = lat * 90 / (1 << 23);
					lng = lng * 360 / (1 << 24);
					
					LOGV("%s: Received position is :%ld %f %f", __FUNCTION__, latSign, lat, lng);
					
					GPS_UBX_AID_INI_U5__t aidingData;
					memset(&aidingData, 0, sizeof(aidingData));
					aidingData.flags = GPS_UBX_AID_INI_U5__FLAGS_POS_MASK | GPS_UBX_AID_INI_U5__FLAGS_LLA_MASK;
					aidingData.ecefXOrLat = (int) (lat * 10000000);
					aidingData.ecefYOrLon = (int) (lng * 10000000);
					
					CUbxGpsState* pUbxGps = CUbxGpsState::getInstance();
					pUbxGps->lock();
					pUbxGps->sendAidingData(&aidingData);
					pUbxGps->unlock();
					
					if (CGpsIf::getInstance()->m_callbacks.location_cb) 
					{
						// Publish
						GpsLocation loc;
						memset(&loc, 0, sizeof(GpsLocation));
						loc.size = sizeof(GpsLocation);
						loc.flags |= GPS_LOCATION_HAS_LAT_LONG;
						loc.latitude = lat;
						loc.longitude = lng;
						CGpsIf::getInstance()->m_callbacks.location_cb(&loc);
					}
				}
				res = -1;
				pHandler->state = END;
			}
			else
			{
				/* unexpected message... */
				LOGV("%s: Timeout %d", __FUNCTION__, cmd);
				LOGV("%s: Sending SUPLEND (Timeout) ====>", __FUNCTION__);
				generateSuplEndMsg(pHandler, StatusCode_unexpectedMessage);
				res = -1;
				pHandler->state = END;
			}
        }
        break;
		
    case RRLP_RESP:
		LOGV("%s: Handling RRLP_RESP", __FUNCTION__);
        if (pMsg == 0)
        {
			// No incoming msg, so expecting a command from device
			char* pSendBuffer = NULL;
			int size = 0;
			
            switch(cmd)
            {
				case SUPL_TIMEOUT:
/*
                    // timeout existing session
					LOGV("%s: Sending SUPLEND (Timeout) ====>", __FUNCTION__);
                    //pSendBuffer = buildRrlpPosRespTimeout(&size, pHandler->rrlpRefNum);
				    logAgps.write(0x22000001, "%d, # timeout", pHandler->sid);
					generateSuplEndMsg(pHandler, StatusCode_protocolError);
					res = -1;
					break;
*/
				
                case SUPL_POS_AVAIL:
                    {
						// position is available and server waiting to receive this
						double lat = 0;
						double lon = 0;
						double alt = 0;
						double tow = 0;
						CMyDatabase* pDatabase = CMyDatabase::getInstance();
						pDatabase->getData(CMyDatabase::DATA_LATITUDE_DEGREES, lat);
						pDatabase->getData(CMyDatabase::DATA_LONGITUDE_DEGREES, lon);
						pDatabase->getData(CMyDatabase::DATA_ALTITUDE_SEALEVEL_METERS, alt);
						pDatabase->getData(CMyDatabase::DATA_UBX_GPSTIME_TOW, tow);
						TIMESTAMP ts;
						memset(&ts, 0, sizeof(ts));
						logAgps.write(0x00000002, "%d, %04d%02d%02d%02d%02d%06.3f,%10.6f,%11.6f,%d #position(time_stamp,lat,lon,ttff)", 
									pHandler->sid, ts.wYear, ts.wMonth, ts.wDay, ts.wHour, ts.wMinute, 1e-6*ts.lMicroseconds, 
									lat, lon, 0); // todo where get the position from ?? 
						pSendBuffer = buildRrlpPosResp(&size, pHandler->rrlpRefNum, lat, lon, alt, tow);
						LOGV("%s: Sending SUPLPOS (position available) ====>", __FUNCTION__);
						res = sendPositionResponse(pHandler, pSendBuffer, size);
						break;
					}
                case SUPL_MSA_DATA_AVAIL:
					// MSA data available and server waiting to receive this
					LOGV("%s: Sending SUPLPOS (msa data) ====>", __FUNCTION__);
					pSendBuffer = buildRrlpMsaPosResp(pHandler->sid, &size, pHandler->rrlpRefNum);
					logAgps.write(0x00000002, "%d, # msa", pHandler->sid);
					res = sendPositionResponse(pHandler, pSendBuffer, size);

					//// No acknowledgement required. Transaction finished - This my change
					//pHandler->state = END;
					//res = -1;
					break;
					
				default:
					assert(0);	// Shouldn't happen
					break;
			}
        }
        break;
		
    case END:
		LOGV("%s: Handling END", __FUNCTION__);
        break;
    }

    LOGV("%s: SM exit with Id = %d in state %s (%d) with value %d", __FUNCTION__, pHandler->sid,stateInfo[pHandler->state],pHandler->state, res);

    return res;
}

///////////////////////////////////////////////////////////////////////////////
//! Allocate a new Supl state handling structure
/*!
  \return Pointer to the new Supl state handling structure. NULL if a failure happens
*/
static suplHandler_t *allocateSM(bool ni)
{
    // Allocate the handler - remember to deallocate in case of error
    suplHandler_t *pHandler = (suplHandler_t*) MC_CALLOC(sizeof(suplHandler_t), 1);
    if (pHandler == NULL)
	{
        LOGV("%s: Error in allocation", __FUNCTION__);
		return NULL;
	}

    pHandler->state = START;								/* set the start of the state machine */
    pHandler->timeout = -1;									/* set the timer initial value */
	pHandler->requestedPosMethod = PosMethod_noPosition;	// No pos method defined
	pHandler->networkInitiated = ni;						// Initiation type
    
    pHandler->pSetId = NULL;
    pHandler->pSlpId = NULL;
    pHandler->pNiMsg = NULL;
	
    return pHandler;		/* return the handler pointer */
}

static bool suplInsertHandler(suplHandler_t *pHandler, const SetSessionID_t *pSetId)
{
	assert(pHandler);
	assert(MAX_SM_INSTANCES < (MAX_SID_NUMBER - START_SID_NUMBER));
	pthread_mutex_lock(&s_handlerMutex);

    if (s_pQueueTail == NULL)
    {
        /* the queue is empty.. */
        s_pQueueTail = pHandler;
    }
    else
    {
        /* browse the queue */
		suplHandler_t *pTmp = s_pQueueTail;
		int cnt = 0;
        while (pTmp->pNext != NULL)
        {
            pTmp = pTmp->pNext;

            /* check number of SM instancies */
            cnt++;
            if (cnt > MAX_SM_INSTANCES)
            {
                /* Error in number of instancies */
				pthread_mutex_unlock(&s_handlerMutex);
                LOGV("%s: too many instancies... %d", __FUNCTION__, cnt);
                return false;
            }
        }
		// Space for insertion
        pTmp->pNext = pHandler;				// Insert
    }

    if (pSetId != NULL)
    {
        pHandler->pSetId = copySetId(pSetId);
        pHandler->sid = pSetId->sessionId;
    }
    else
    {
        pHandler->sid = getNewSid();		// Get a unique sid - NB handler list mutex is locked
        pHandler->pSetId = fillDefaultSetId(pHandler->sid);
    }

	pthread_mutex_unlock(&s_handlerMutex);

	return true;
}

///////////////////////////////////////////////////////////////////////////////
//! Releases a Supl state handling structure
/*!
  \param pHandler : Pointer to the Supl state handling structure to release
*/
static void freeSuplHandler(suplHandler_t *pHandler)
{
	assert(pHandler);
	assert(pthread_self() == g_gpsDrvMainThread);
	pthread_mutex_lock(&s_handlerMutex);

    /* Check the consistency of the queue */
    assert(s_pQueueTail);

    /* close the socket, if it is open... */
	if (pHandler->bio != 0)
	{
        CAgpsIf* pAgps = CAgpsIf::getInstance();

        /*TODO if (pAgps->isTlsActive())
        {
            SSL *pSsl = NULL;
            BIO_get_ssl(pHandler->bio, &pSsl);
            if (pSsl != NULL)
            {
                SSL_shutdown(pSsl);
            }
            else
            {
                LOGW("%s: ssl instance is NULL", __FUNCTION__);
            }
        }*/
		BIO_free_all(pHandler->bio);
		SSL_CTX_free(pHandler->ctx); 
		LOGV("%s: closed socket", __FUNCTION__);
	}

    /* check if it's the first element */
	suplHandler_t *pTmp = s_pQueueTail;
    if (pTmp == pHandler)
    {
        s_pQueueTail = pHandler->pNext;
    }
	else
	{
		while (pTmp->pNext != pHandler)
		{
			/* Check the consistency of the queue */
			assert(pTmp->pNext != NULL);

			/* Browse the tree to get the right handler */
			pTmp = pTmp->pNext;
		}

		/* shift the elements */
		pTmp->pNext = pHandler->pNext;
	}
	
	pthread_mutex_unlock(&s_handlerMutex);

    LOGV("%s: deallocate handler", __FUNCTION__);

    /* deallocate it */
    safeFreeHandler(pHandler);

    LOGV("%s: handler deallocated", __FUNCTION__);
}   

///////////////////////////////////////////////////////////////////////////////
//! Find an existing Supl state handling structure
/*! Find an existing Supl state handling structure based on the Session ID of
    an ongoing Supl transaction
  \param sid : Session ID
  \return Pointer to the Supl state handling structure with matching session ID.
          NULL if no match found.
*/
static suplHandler_t *searchSMInst(int sid, bool lock)
{
	if (lock) 
    {
        pthread_mutex_lock(&s_handlerMutex);
	
        /* take a pointer to the tail */
        suplHandler_t *pTmp = s_pQueueTail;

        while(pTmp != NULL)
        {
            if (pTmp->sid == sid)
            {
                /* found the correct one */
                break;
            }
            /* browse the queu */
            pTmp = pTmp->pNext;
        }
	
        pthread_mutex_unlock(&s_handlerMutex);
        /* no instance available with the sid */
        return pTmp;
    }
    else
    {
        /* take a pointer to the tail */
        suplHandler_t *pTmp = s_pQueueTail;

        while(pTmp != NULL)
        {
            if (pTmp->sid == sid)
            {
                /* found the correct one */
                break;
            }
            /* browse the queu */
            pTmp = pTmp->pNext;
        }
	
        /* no instance available with the sid */
        return pTmp;
    }
}

///////////////////////////////////////////////////////////////////////////////
//! Generates a new Session ID
/*! 
  \return New session ID
*/
static int getNewSid(void)
{
    /* Start with sid = START_SID_NUMBER */
    static int ret = START_SID_NUMBER;

    /* Use the search function.. */
    do
    {
        ret++;
        if (ret > MAX_SID_NUMBER)
        {
            ret = START_SID_NUMBER;
        }
    }
    while (searchSMInst(ret, false) != NULL);
    return ret;
}

///////////////////////////////////////////////////////////////////////////////
//! Generate a new Supl session
/*! Generates a new Supl session by setting up a socket connection to a the Supl
    server and filling the supplied supl state structure with appropriate session
    information
  \param pHandler : Pointer to Supl state structure
  \return 0 id successful, < 0 if failed
*/
static int createUplSession(suplHandler_t *pHandler)
{
	assert(pHandler);
	assert(pthread_self() == g_gpsDrvMainThread);
	
	CAgpsIf* pAgps = CAgpsIf::getInstance();
    char tmpString[50];
    SSL * ssl;
    BIO *bio;

    /* prepare the address */
	int suplPort;
	char* pSuplServerAddress;
	pAgps->getSuplServerInfo(&pSuplServerAddress, &suplPort);
    snprintf(tmpString, sizeof(tmpString), "%s:%d", pSuplServerAddress, suplPort);

    if (pAgps->isTlsActive())
    {
        LOGV("%s: **SECURE** TLS connection", __FUNCTION__);
        /* Set up the SSL context */
        if ( (pHandler->ctx = SSL_CTX_new(SSLv23_client_method())) == NULL)
        {
            LOGE("%s: Error in creation of new context: %s", __FUNCTION__, ERR_error_string(ERR_get_error(),NULL));
			return -1;
        }

        /* Load the trust store */
		const char* certFileName = pAgps->getCertificateFileName();
		assert(certFileName);
		
        if(*certFileName)
        {
            LOGV("%s: Loading the certificate %s", __FUNCTION__, certFileName);

            if(!SSL_CTX_load_verify_locations(pHandler->ctx, certFileName, NULL))
            {
                LOGE("%s: Error loading trust store %s", __FUNCTION__, certFileName);
                return -1;
            }

            LOGV("%s: Certificate loaded", __FUNCTION__);
        }

        /* Setup the connection, store temporary locally */
        /*TODO if ( (bio = BIO_new_ssl_connect(pHandler->ctx)) == NULL)
        {
            LOGE("%s: Cannot connect", __FUNCTION__);
			return -1;
        }*/

        /* Set the SSL_MODE_AUTO_RETRY flag */
        /*TODO BIO_get_ssl(bio, &ssl);
        SSL_set_mode(ssl, SSL_MODE_AUTO_RETRY);
	*/
        /* Create and setup the connection */
        //TODO BIO_set_conn_hostname(bio, tmpString);

	BIO_new_connect(tmpString);

        if (connectTimeout(bio, pHandler->sid, 5) == -1)
        {
			LOGV("%s: TLS Session connection failed", __FUNCTION__);
            return -1;
        }

        /* Check the certificate */
		if (*certFileName)
		{
			long res = SSL_get_verify_result(ssl);
			if(res != X509_V_OK)
			{
				LOGE("%s: Certificate verification error: %ld", __FUNCTION__, res);
				return -1;
			}
		}

        LOGV("%s: TLS Session established...", __FUNCTION__);
        /* update the handler */
        pHandler->bio = bio;
    }
    else
    {
        LOGV("%s: Non TLS connection to %s", __FUNCTION__, tmpString);
		pHandler->bio = BIO_new_connect(tmpString);

		if (connectTimeout(pHandler->bio, pHandler->sid, 5) == -1)
        {
			LOGV("%s: Non TLS Session connection failed", __FUNCTION__);
            pHandler->bio = NULL;
            return -1;
        }

        LOGV("%s: Non TLS Session established...", __FUNCTION__);
    }

    return 0;
}

///////////////////////////////////////////////////////////////////////////////
//! Calculate a hash based on the contents of the supplied buffer
/*! 
  \param buffer : Pointer to buffer
  \param size   : Size of buffer
  \return 64 bit hash value
*/
static long long calculateHash(const char *buffer, int size)
{
	assert(buffer);
	
    unsigned char *returnCode;
    long long res;
    unsigned int sizeOut;
    unsigned char outHash[EVP_MAX_MD_SIZE];
	CAgpsIf* pAgps = CAgpsIf::getInstance();
	int suplPort;
#if defined SUPL_FQDN_SLP
	char pSuplServerAddress[] = SUPL_FQDN_SLP;
#else
	char* pSuplServerAddress;
	pAgps->getSuplServerInfo(&pSuplServerAddress, &suplPort);
#endif
		
    LOGV("%s: Size of the HMAC input buffer is %d", __FUNCTION__, size);
    LOGV("%s: the key is %s", __FUNCTION__, pSuplServerAddress);
    LOGV("%s: the key length is %zd", __FUNCTION__, strlen(pSuplServerAddress));
    returnCode = HMAC(EVP_sha1(),
                      pSuplServerAddress,
                      (int) strlen(pSuplServerAddress),
                      (unsigned char *) const_cast<char *>(buffer),
                      (unsigned int) size,
                      outHash,
                      &sizeOut);
    LOGV("%s: size of the hash = %d", __FUNCTION__, size);

    memcpy(&res, returnCode, sizeof(res));
    LOGV("%s: The calculated HASH is: %.16LX", __FUNCTION__, res);

    return res;
}

///////////////////////////////////////////////////////////////////////////////
//! Connects to Supl server
/*! 
  \param pHandler : Pointer to Supl state structure
  \param seconds  : Maximum time to spend trying to connect
  \return 0 if connection is successful, < 0 if not
*/
static int connectTimeout(BIO *bio, int sid, int seconds)
{
	assert(bio);

    ((void)(seconds));
	
    BIO_set_nbio(bio, 1);		// Set to non blocking
    //lint -e{522} remove Highest operation, a 'cast', lacks side-effects
    BIO_set_close(bio, BIO_CLOSE);
	
	do
    {
        int fd;
		fd_set          readfds;
        fd_set          writefds;
        struct timeval  tv;
        int err;

		LOGV("%s # server connecting...", __FUNCTION__);
		logAgps.write(0x10000002, "%d # server connecting...", sid);
        if (BIO_do_connect(bio) == 1) 
		{
			break;          /* Connection established */
        }
        
		if (! BIO_should_retry(bio)) 
		{ 
			/* Hard error? */
            LOGE("%s: Cannot connect", __FUNCTION__);
            BIO_free_all(bio);
			logAgps.write(0x21000001, "%d # no network", sid);
            return -1;
        }
		
        /* Wait until there's a change in the socket's status or until
         * the timeout period.
         *
         * Get underlying file descriptor, needed for the select call.
         */
        fd = BIO_get_fd(bio, NULL);
        if (fd == -1) 
		{
            LOGE("%s: BIO isn't initialized in oh_ssl_connect()", __FUNCTION__);
        }
        assert(fd != -1);

//lint -e{866} suppress "Unusual use of '' in argument to sizeof"
        FD_ZERO(&readfds);
//lint -e{866} suppress "Unusual use of '' in argument to sizeof"
        FD_ZERO(&writefds);
        if (BIO_should_read(bio)) 
		{
			FD_SET(fd, &readfds);
        }
        else if (BIO_should_write(bio)) 
		{
            FD_SET(fd, &writefds);
        }
        else 
		{                  
			/* This is BIO_should_io_special().
			 * Not sure what "special" needs to
			 * wait for, but both read and write
			 * seems to work without unnecessary
			 * retries.
			 */
            FD_SET(fd, &readfds);
            FD_SET(fd, &writefds);
        }

        tv.tv_sec = 5;
        tv.tv_usec = 0;
        err = select(fd+1, &readfds, &writefds, NULL, &tv);

        /* Evaluate select() return code */
        if (err <= 0) 
		{
			if (err < 0) 
			{
				LOGE("%s: error during select()", __FUNCTION__);
			}
			else
			{
				LOGV("%s: connection timeout", __FUNCTION__);
			}
            BIO_free_all(bio);
			logAgps.write(0x21000001, "%d # tls failed", sid);
			return -1;   /* Timeout */
        }
    }
    while(1);

	logAgps.write(0x10000003, "%d # server connection success", sid);
    return 0;
}

///////////////////////////////////////////////////////////////////////////////
//! Generate and send a Supl End message to server
/*! 
  \param pHandler   : Pointer to Supl state structure
  \param statusCode : Status code to include in Supl End message
*/
static void generateSuplEndMsg(suplHandler_t * pHandler, StatusCode statusCode)
{
	assert(pHandler);
	suplEndParam_t par;

	par.pSetId = pHandler->pSetId;
	par.pSlpId = pHandler->pSlpId;
	par.posEn = 0;
	
	if (pHandler->hash == 0)
	{
		par.verEn = 0;
	}
	else
	{
		par.verEn = 1;
	}
	par.hash = pHandler->hash;
	
	/* Error code: not specified */
	par.status = statusCode;

	/* we must send the SUPL_END, and then close the connection */
	sendSuplEnd(pHandler->bio, &par);

	/* Next state: END */
	pHandler->state = END;
}

///////////////////////////////////////////////////////////////////////////////
//! Generate and send a Supl Position Response message to server
/*! 
  \param pHandler    : Pointer to Supl state structure
  \param pSendBuffer : Pointer to the RRLP encodes message which will be the payload for the Supl message
  \param size        : Size of RRLP message buffer
  \return  0 successful, < 0 session is finished
*/
static int sendPositionResponse(suplHandler_t *pHandler, char* pSendBuffer, int size)
{
	assert(pHandler);

	int res = 0;

	if (pSendBuffer == NULL)
	{
		// Only response is to end session
		generateSuplEndMsg(pHandler, StatusCode_protocolError);
		res = -1;
	}
	else
	{
		suplPosParam_t par;
		memset(&par, 0, sizeof(par));
		
		// Send position response
		par.pSlpId = pHandler->pSlpId;
		par.pSetId = pHandler->pSetId;
		par.buffer = pSendBuffer;
		par.size = size;
	
		/* Ok, this is what we expect... */
		sendSuplPos(pHandler->bio, &par);

		/* Set the timeout */
		pHandler->timeout = time(NULL) + SUPL_STD_TIMEOUT;
		pHandler->state = RRLP;
		
		MC_FREE(pSendBuffer);
	}
	
	return res;
}

///////////////////////////////////////////////////////////////////////////////
//! Starts a Ni Supl session
/*! Starts a Ni Supl session by preparing a notify/verify request which is sent
    to the framework. If the request is accepted, the framework will callback to
	the driver to continue the Supl session process.
  \param pHandler    : Pointer to Supl state structure
*/
static void startNiSession(suplHandler_t *pHandler)
{
	assert(pHandler);
	assert(pthread_self() == g_gpsDrvMainThread);
	
	logAgps.write(0x01000000, "%d # network connection success", pHandler->sid);
	// wait authorization or timeout
	pHandler->state = AUTH;

	CNiIf* pNiIf = CNiIf::getInstance();
	
	GpsNiNotification notification;
	memset(&notification, 0, sizeof(GpsNiNotification));
	
	notification.size = sizeof(GpsNiNotification);
	notification.notification_id = pHandler->sid;
    notification.ni_type = GPS_NI_TYPE_UMTS_SUPL;		// TODO - Is this the right type?
	
	setNotificationAndResponse(pHandler->notificationType, 
		&notification.notify_flags, 
		&notification.default_response);
    notification.timeout = CUbxGpsState::getInstance()->getNiUiTimeout();
	LOGV("%s: notify timeout %d", __FUNCTION__, notification.timeout);
	
    if (pHandler->pRequestorId != NULL)
    {
        copyAndConvertText(notification.requestor_id, GPS_NI_SHORT_STRING_MAXLEN, pHandler->pRequestorId);
    }
    else
    {
        notification.requestor_id[0] = '\0';
    }

    if (pHandler->pClientName != NULL)
    {
        copyAndConvertText(notification.text, GPS_NI_LONG_STRING_MAXLEN, pHandler->pClientName);
    }
    else
    {
        notification.text[0] = '\0';
    }
	
    notification.requestor_id_encoding = convertAsn1Encoding(pHandler->encodingType);
    notification.text_encoding = notification.requestor_id_encoding;
	// No 'notification.extras' need to be added
	
	CNiIf::request(&notification);		// This will cause a recursive call to suplSM
										// pHandler could be now be a floating pointer
										// if error occured in recursed suplSm
										
	// Be sure the request has complete the operations 
	sem_wait(&pNiIf->sem);

	// here we should be sure the notification is completed
	suplHandleAuthorization(notification.notification_id, pNiIf->m_cmd);

}

///////////////////////////////////////////////////////////////////////////////
//! Converts a Supl text to the 'hex' representation needed by the framework
/*! 
  \param pDest       : Pointer the destination buffer
  \param maxLen      : Size of the destination buffer
  \param pSuplString : Pointer to Supl text to convert
*/
static void copyAndConvertText(char* pDest, int maxLen, const OCTET_STRING_t* pSuplString)
{
	assert(pDest);
	assert(pSuplString);
	
	int len = pSuplString->size;
	uint8_t* pSrc = pSuplString->buf;
	
	if (len * 2 >= maxLen)
	{
		len = (maxLen / 2) - 1;
	}
	
	for(int i = 0; i < len; i++)
	{
		*pDest = (*pSrc >> 4);
		*pDest += *pDest > 9 ? 55 : 48;
		pDest++;
		
		*pDest = (*pSrc & 0x0F);
		*pDest += *pDest > 9 ? 55 : 48;
		pDest++;
		pSrc++;
	}
	
	*pDest = 0;
}

///////////////////////////////////////////////////////////////////////////////
//! Convert Supl encoding to framework encoding
/*! Function to convert a Supl text encoding type into a text encoding type 
    used by the framework
  \param encodingType : Supl encoding type
  \return Framework encoding type
*/
static GpsNiEncodingType convertAsn1Encoding(EncodingType_t encodingType)
{
	GpsNiEncodingType niEncodingType = GPS_ENC_UNKNOWN;
	
	switch(encodingType)
	{
	case EncodingType_ucs2:
		niEncodingType = GPS_ENC_SUPL_UCS2;
		break;
		
	case EncodingType_gsmDefault:
		niEncodingType = GPS_ENC_SUPL_GSM_DEFAULT;
		break;
		
	case EncodingType_utf8:
		niEncodingType = GPS_ENC_SUPL_UTF8;
		break;
		
	default:
		break;
	}
	
	return niEncodingType;
}

///////////////////////////////////////////////////////////////////////////////
//! Converts a Supl notify/verify request to framework equivalent
/*! Converts a Supl notify/verify request to framework equivalent in the form 
    of Notify flags and default response
  \param ans1cNotifyType  : Supl nofity/verify request 
  \param pNotifyFlags     : Pointer to framework notify flags
  \param pDefaultResponse : Pointer to framework default response
*/
static void setNotificationAndResponse(NotificationType_t ans1cNotifyType, 
	GpsNiNotifyFlags* pNotifyFlags, 
	GpsUserResponseType* pDefaultResponse)
{
	assert(pNotifyFlags);
	assert(pDefaultResponse);
	
	*pNotifyFlags = 0;
	*pDefaultResponse = GPS_NI_RESPONSE_NORESP;
	
	switch(ans1cNotifyType)
	{
	case NotificationType_noNotificationNoVerification:
		// No need to set any flags
		break;
		
	case NotificationType_notificationOnly:
		*pNotifyFlags = GPS_NI_NEED_NOTIFY;
		break;
	
	case NotificationType_notificationAndVerficationAllowedNA:
		*pNotifyFlags = GPS_NI_NEED_NOTIFY | GPS_NI_NEED_VERIFY;
		*pDefaultResponse = GPS_NI_RESPONSE_ACCEPT;
		break;
		
	case NotificationType_notificationAndVerficationDeniedNA:
		*pNotifyFlags = GPS_NI_NEED_NOTIFY | GPS_NI_NEED_VERIFY;
		*pDefaultResponse = GPS_NI_RESPONSE_DENY;
		break;
		
	case NotificationType_privacyOverride:
		*pNotifyFlags = GPS_NI_PRIVACY_OVERRIDE;
		break;

	default:
		LOGV("CNiIf::%s: Can't translate ans1cNotifyType(%li)", __FUNCTION__, ans1cNotifyType);
		break;
	}
}

///////////////////////////////////////////////////////////////////////////////
//! Determines if a Supl transaction can be started or allowed to proceeed
/*! 
  \return : true if Supl transaction can start/proceed. false if not
*/
static bool isSuplPossible(void)
{
	int suplPort;
	char* pSuplServerAddress;
	CAgpsIf::getInstance()->getSuplServerInfo(&pSuplServerAddress, &suplPort);
	if ((pSuplServerAddress == NULL) || (suplPort < 0))
	{
		// No Supl server specified.
		LOGE("%s: No server specified", __FUNCTION__);
		return false;
	}

	return true;
}

///////////////////////////////////////////////////////////////////////////////
//! Determine if assistance dataneeds to be requested from Supl server
/*! 
  \param pHandler		: Pointer to Supl session state structure
  \param pPosInitParams	: Pointer to SuplPosInit message parameters structure to populate
*/
static void determineAssistance(suplHandler_t* pHandler, suplPosInitParam_t* pPosInitParams)
{
	assert(pHandler);
	assert(pPosInitParams);
	
	double lat, lon;
	
	CMyDatabase* pDatabase = CMyDatabase::getInstance();
	assert(pDatabase);
	
	if (pDatabase->getData(CMyDatabase::DATA_LATITUDE_DEGREES, lat) && 
		pDatabase->getData(CMyDatabase::DATA_LONGITUDE_DEGREES, lon))
	{
		// Don't need assistance as we already have position
		pPosInitParams->posEn = 1;
		pPosInitParams->lat = lat;
		pPosInitParams->lon = lon;
		pPosInitParams->assEn = 0;
		pHandler->assistanceRequested = false;
	}
	else
	{
		// Don't have position, request assistance
		pPosInitParams->posEn = 0;
		pPosInitParams->assEn = 1;
		pHandler->assistanceRequested = true;
	}
}

///////////////////////////////////////////////////////////////////////////////
//! Starts a Si Supl session
/*! 
  \param pHandler    : Pointer to Supl state structure
*/
static int startSiSession(suplHandler_t* pHandler)
{
	assert(pHandler);
	assert(pthread_self() == g_gpsDrvMainThread);
	
	logAgps.write(0x01000000, "%d # network connection success", pHandler->sid);
	int res = 0; // success by default;
	
	/* Create a UPL session */
	if (createUplSession(pHandler) == -1)
	{
		pHandler->state = END;
		res = -1;
	}
	else
	{
		LOGV("%s: Sending SUPLSTART ====>", __FUNCTION__);
		sendSuplStart(pHandler->bio, pHandler->pSetId);
		/* Next state */
		pHandler->state = WAIT_RES;
		/* set the timeout */
		pHandler->timeout = time(NULL) + SUPL_STD_TIMEOUT;
	}
	
	return res;
}

///////////////////////////////////////////////////////////////////////////////
//! Perform any clean up required when ending a Supl session
/*! 
  \param pHandler    : Pointer to Supl state structure
*/
static void endSuplSession(suplHandler_t* pHandler)
{
	assert(pHandler);
	assert(pthread_self() == g_gpsDrvMainThread);
	
	logAgps.write(0x00000001, "%d # a-gps session ends", pHandler->sid);
	
	if (pHandler->networkInitiated)
	{
		// End of NI session - signal engine to stop
		if (s_pGpsControlInterface)
		{
			s_pGpsControlInterface->requestStop_cb(s_pEventContext);
		}
	}
	else
	{
		// End of SI session - Allow publish if MSA
		if (CGpsIf::getInstance()->getMode() == GPS_POSITION_MODE_MS_ASSISTED)
		{
			CMyDatabase::getInstance()->incPublish();
		}
	}
	
	freeSuplHandler(pHandler);
}

///////////////////////////////////////////////////////////////////////////////
//! Verify consistency of slp session ID
/*! 
  \param pSlpSessionId_1   : Pointer to slp session Id
  \param pSlpSessionId_2   : Pointer to slp session Id
  \return                  : true if consistent
*/
static bool verifySlpSessionId(SlpSessionID_t *pSlpSessionId_1, SlpSessionID_t *pSlpSessionId_2)
{
    if (pSlpSessionId_1 == NULL && pSlpSessionId_2 == NULL)
        return true;
    else if (pSlpSessionId_1 == NULL || pSlpSessionId_2 == NULL)
        return false;
    else 
    {
        if (!compareOctets(&pSlpSessionId_1->sessionID, &pSlpSessionId_2->sessionID))
            return false;

        if (pSlpSessionId_1->slpId.present != pSlpSessionId_2->slpId.present)
            return false;

        if (pSlpSessionId_1->slpId.present == SLPAddress_PR_iPAddress)
        {
            if (pSlpSessionId_1->slpId.choice.iPAddress.present != pSlpSessionId_2->slpId.choice.iPAddress.present)
                return false;

            if (pSlpSessionId_1->slpId.choice.iPAddress.present == IPAddress_PR_ipv4Address)
            {
                OCTET_STRING_t *pOct1 = &pSlpSessionId_1->slpId.choice.iPAddress.choice.ipv4Address;
                OCTET_STRING_t *pOct2 = &pSlpSessionId_2->slpId.choice.iPAddress.choice.ipv4Address;
                if (!compareOctets(pOct1, pOct2))
                    return false;

            }

            if (pSlpSessionId_1->slpId.choice.iPAddress.present == IPAddress_PR_ipv6Address)
            {
                OCTET_STRING_t *pOct1 = &pSlpSessionId_1->slpId.choice.iPAddress.choice.ipv6Address;
                OCTET_STRING_t *pOct2 = &pSlpSessionId_2->slpId.choice.iPAddress.choice.ipv6Address;
                if (!compareOctets(pOct1, pOct2))
                    return false;

            }
        }
        if (pSlpSessionId_1->slpId.present == SLPAddress_PR_fQDN)
        {
            OCTET_STRING_t *pOct1 = &pSlpSessionId_1->slpId.choice.fQDN;
            OCTET_STRING_t *pOct2 = &pSlpSessionId_2->slpId.choice.fQDN;
            if (!compareOctets(pOct1, pOct2))
                return false;
        }
    }
    return true;        
}

///////////////////////////////////////////////////////////////////////////////
//! Verify consistency of set session ID
/*! 
  \param pSetSessionId_1   : Pointer to set session Id
  \param pSetSessionId_2   : Pointer to set session Id
  \return                  : true if consistent
*/
static bool verifySetSessionId(const SetSessionID_t *pSetSessionId_1, const SetSessionID_t *pSetSessionId_2)
{
    if (pSetSessionId_1 == NULL && pSetSessionId_2 == NULL)
        return true;

    if (pSetSessionId_1 == NULL || pSetSessionId_2 == NULL)
        return false;

    if (pSetSessionId_1->sessionId != pSetSessionId_2->sessionId)
        return false;

    if (pSetSessionId_1->setId.present != pSetSessionId_2->setId.present)
        return false;

    /* TODO add also check if the content of setId is consistent... */

    return true;        
}

///////////////////////////////////////////////////////////////////////////////
//! Compare cotets
/*! 
  \param pOct1   : Pointer to octet
  \param pOct2   : Pointer to octet
  \return        : true if equal
*/
static bool compareOctets(const OCTET_STRING_t *pOct1, const OCTET_STRING_t *pOct2)
{
    int i;
    if (pOct1->size != pOct2->size)
        return false;
    for (i=0; i<pOct1->size; i++)
    {
        if (pOct1->buf[i] != pOct2->buf[i])
            return false;
    }

    return true;
}


static void safeFreeHandler(suplHandler_t* pHandler)
{
    /* deallocate the pSetId if allocated */
    if (pHandler->pSetId != NULL)
    {
        ASN_STRUCT_FREE(asn_DEF_SetSessionID, pHandler->pSetId);
        pHandler->pSetId = NULL;
    }

    /* deallocate the pSlpId if allocated */
    if (pHandler->pSlpId != NULL)
    {
        ASN_STRUCT_FREE(asn_DEF_SlpSessionID, pHandler->pSlpId);
        pHandler->pSlpId = NULL;
    }

    /* deallocate the pNiMsg */
    if (pHandler->pNiMsg != NULL)
    {
        ASN_STRUCT_FREE(asn_DEF_ULP_PDU, pHandler->pNiMsg);
        pHandler->pNiMsg = NULL;
    }


    /* Deallocating the hanlder itself */
    MC_FREE(pHandler);
}
