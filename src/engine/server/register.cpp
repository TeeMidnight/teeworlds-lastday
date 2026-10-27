/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <base/math.h>
#include <base/system.h>
#include <engine/console.h>
#include <engine/engine.h>
#include <engine/message.h>
#include <engine/shared/config.h>
#include <engine/shared/http_request.h>
#include <engine/shared/jsonparser.h>
#include <engine/shared/network.h>

#include "register.h"

#include <mastersrv/mastersrv.h>

// Seconds between two registrations of the same protocol. A master server
// entry expires when it is not refreshed for a while, so this must stay well
// below the master's timeout.
static const int REGISTER_INTERVAL = 15;
// Unregistering on shutdown must not delay the process exit for long.
static const int SHUTDOWN_DELETE_TIMEOUT = 2;
static const int DELETE_TIMEOUT = 5;

static const char *NextToken(const char *pStr, char *pOut, int OutSize)
{
	if(!pStr)
		return nullptr;
	while(*pStr == ' ' || *pStr == '\t')
		pStr++;
	const char *pEnd = pStr;
	while(*pEnd && *pEnd != ',')
		pEnd++;
	int Length = minimum((int) (pEnd - pStr), OutSize - 1);
	mem_copy(pOut, pStr, Length);
	while(Length > 0 && (pOut[Length - 1] == ' ' || pOut[Length - 1] == '\t'))
		Length--;
	pOut[Length] = '\0';
	// Returns nullptr after the last token, but the token itself is already
	// stored in pOut, so callers must process it before retrying.
	return *pEnd ? pEnd + 1 : nullptr;
}

void CRegister::CheckChallengeStatus(int Protocol)
{
	CProtocol &P = m_aProtocols[Protocol];
	lock_wait(P.m_Lock);
	if(P.m_LastResponseIndex == P.m_NumTotalRequests - 1)
	{
		switch(P.m_LastResponseStatus)
		{
			case STATUS_NEEDCHALLENGE:
				if(P.m_NewChallengeToken)
				{
					// Immediately resend if we got the token.
					P.m_NextRegister = time_get();
				}
				break;
			case STATUS_NEEDINFO:
				// Act immediately if the master requests more info.
				P.m_NextRegister = time_get();
				break;
		}
	}
	lock_unlock(P.m_Lock);
}

void CRegister::UpdateRegister(int Protocol)
{
	CheckChallengeStatus(Protocol);

	CProtocol &P = m_aProtocols[Protocol];
	if(time_get() < P.m_NextRegister)
		return;

	// Reap finished jobs. Only the main thread ever touches this list, and the
	// pool never looks at a job again once it marked it as done.
	for(int i = 0; i < P.m_lpJobs.size();)
	{
		if(P.m_lpJobs[i]->Status() == CJob::STATE_DONE)
		{
			delete P.m_lpJobs[i];
			P.m_lpJobs.remove_index_fast(i);
		}
		else
		{
			i++;
		}
	}

	// Rate limit before handing the request off, so a slow master cannot pile
	// up one request per tick.
	const int64 Now = time_get();
	P.m_NewChallengeToken = false;
	P.m_PrevRegister = Now;
	P.m_NextRegister = Now + REGISTER_INTERVAL * time_freq();

	int Index = P.m_lpJobs.add(new CJob());
	Engine()->AddJob(P.m_lpJobs[Index], CRegister::SendRegister, &P.m_Context);
}

int CRegister::SendRegister(void *pUser)
{
	CProtocol::CRegisterContext *pContext = static_cast<CProtocol::CRegisterContext *>(pUser);
	CRegister *pSelf = pContext->m_pParent;
	const int Protocol = pContext->m_Protocol;
	CProtocol &P = pSelf->m_aProtocols[Protocol];

	char aConnlessTokenHex[sizeof(pSelf->m_aConnlessTokenHex)];
	char aCommunityToken[sizeof(pSelf->m_aCommunityToken)];
	char aChallengeToken[sizeof(P.m_aChallengeToken)];
	char aaExtraHeaders[MAX_EXTRA_HEADERS][128];
	int NumExtraHeaders;
	bool HaveChallengeToken;
	int InfoSerial;
	bool SendInfo;
	int ServerPort;

	// Everything below lives in the main thread, so take a snapshot instead of
	// reading it while the main thread may be rewriting it.
	{
		lock_wait(pSelf->m_Lock);
		str_copy(aConnlessTokenHex, pSelf->m_aConnlessTokenHex, sizeof(aConnlessTokenHex));
		str_copy(aCommunityToken, pSelf->m_aCommunityToken, sizeof(aCommunityToken));
		NumExtraHeaders = pSelf->m_NumExtraHeaders;
		for(int i = 0; i < NumExtraHeaders; i++)
			str_copy(aaExtraHeaders[i], pSelf->m_aaExtraHeaders[i], sizeof(aaExtraHeaders[i]));
		InfoSerial = pSelf->m_InfoSerial;
		SendInfo = InfoSerial > pSelf->m_LastSuccessfulInfoSerial;
		ServerPort = pSelf->m_ServerPort;
		lock_unlock(pSelf->m_Lock);
	}
	{
		lock_wait(P.m_Lock);
		HaveChallengeToken = P.m_HaveChallengeToken;
		str_copy(aChallengeToken, P.m_aChallengeToken, sizeof(aChallengeToken));
		lock_unlock(P.m_Lock);
	}

	char aAddress[64];
	str_format(aAddress, sizeof(aAddress), "%sconnecting-address.invalid:%d", ProtocolToScheme(Protocol), ServerPort);

	char aSecret[UUID_MAXSTRSIZE];
	format_uuid(pSelf->m_Secret, aSecret, sizeof(aSecret));

	char aChallengeUuid[UUID_MAXSTRSIZE];
	format_uuid(pSelf->m_ChallengeSecret, aChallengeUuid, sizeof(aChallengeUuid));

	char aChallengeSecret[64];
	str_format(aChallengeSecret, sizeof(aChallengeSecret), "%s:%s", aChallengeUuid, ProtocolToString(Protocol));

	CHttpRequest Register("POST", pSelf->Config()->m_SvRegisterUrl, 15L, Protocol == PROTOCOL_IPV4 ? HTTP_IPRESOLVE_IPV4ONLY : HTTP_IPRESOLVE_IPV6ONLY);
	if(SendInfo)
	{
		// PostJson() copies the JSON, so the lock is only held for the copy.
		lock_wait(pSelf->m_Lock);
		Register.PostJson(pSelf->m_aServerInfo);
		lock_unlock(pSelf->m_Lock);
	}
	char aHeader[256];
	str_format(aHeader, sizeof(aHeader), "Address: %s", aAddress);
	Register.AddHeader(aHeader);

	str_format(aHeader, sizeof(aHeader), "Secret: %s", aSecret);
	Register.AddHeader(aHeader);

	str_format(aHeader, sizeof(aHeader), "Connless-Token: %s", aConnlessTokenHex);
	Register.AddHeader(aHeader);

	str_format(aHeader, sizeof(aHeader), "Challenge-Secret: %s", aChallengeSecret);
	Register.AddHeader(aHeader);
	if(HaveChallengeToken)
	{
		str_format(aHeader, sizeof(aHeader), "Challenge-Token: %s", aChallengeToken);
		Register.AddHeader(aHeader);
	}
	str_format(aHeader, sizeof(aHeader), "Info-Serial: %d", InfoSerial);
	Register.AddHeader(aHeader);

	if(aCommunityToken[0])
	{
		str_format(aHeader, sizeof(aHeader), "Community-Token: %s", aCommunityToken);
		Register.AddHeader(aHeader);
	}
	for(int i = 0; i < NumExtraHeaders; i++)
		Register.AddHeader(aaExtraHeaders[i]);

	int RequestIndex;
	{
		lock_wait(P.m_Lock);
		if(P.m_LastResponseStatus != STATUS_OK)
		{
			pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, ProtocolToSystem(Protocol), "registering...");
		}
		RequestIndex = P.m_NumTotalRequests;
		P.m_NumTotalRequests++;
		lock_unlock(P.m_Lock);
	}

	Register.StartRunBlocking();

	char aBuf[256];
	if(Register.Result() != 0)
	{
		// TODO: exponential backoff
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_ADDINFO, ProtocolToSystem(Protocol), "error sending request to master");
		return -1;
	}
	CJsonParser Parser;
	json_value *pJson = Parser.ParseData(Register.ReceivedData(), Register.ReceivedDataSize());
	if(!pJson)
	{
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_ADDINFO, ProtocolToSystem(Protocol), "non-JSON response from master");
		return -2;
	}
	const json_value &rStatusString = (*pJson)["status"];
	if(rStatusString.type != json_string)
	{
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_ADDINFO, ProtocolToSystem(Protocol), "invalid JSON response from master");
		return -3;
	}
	int Status;
	if(StatusFromString(&Status, rStatusString))
	{
		str_format(aBuf, sizeof(aBuf), "invalid status from master: %s", (const char *) rStatusString);
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_ADDINFO, ProtocolToSystem(Protocol), aBuf);
		return -4;
	}
	if(Status == STATUS_ERROR)
	{
		const json_value &rMessage = (*pJson)["message"];
		if(rMessage.type != json_string)
		{
			pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_ADDINFO, ProtocolToSystem(Protocol), "invalid JSON error response from master");
			return -5;
		}
		str_format(aBuf, sizeof(aBuf), "error response from master: %d: %s", Register.ResponseCode(), (const char *) rMessage);
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_ADDINFO, ProtocolToSystem(Protocol), aBuf);
		return -6;
	}
	if(Register.ResponseCode() >= 400)
	{
		str_format(aBuf, sizeof(aBuf), "non-success status code %d from master without error code", Register.ResponseCode());
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_ADDINFO, ProtocolToSystem(Protocol), aBuf);
		return -7;
	}
	{
		lock_wait(P.m_Lock);
		if(Status != P.m_LastResponseStatus)
		{
			if(Status != STATUS_OK)
			{
				if(pSelf->Config()->m_Debug)
					dbg_msg(ProtocolToSystem(Protocol), "status: %s", (const char *) rStatusString);
			}
			else
			{
				pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_ADDINFO, ProtocolToSystem(Protocol), "successfully registered");
			}
		}
		if(Status == P.m_LastResponseStatus && Status == STATUS_NEEDCHALLENGE)
		{
			pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_ADDINFO, ProtocolToSystem(Protocol), "ERROR: the master server reports that clients can not connect to this server.");
			str_format(aBuf, sizeof(aBuf), "ERROR: configure your firewall/nat to let through udp on port %d.", ServerPort);
			pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_ADDINFO, ProtocolToSystem(Protocol), aBuf);
		}
		if(RequestIndex > P.m_LastResponseIndex)
		{
			P.m_LastResponseIndex = RequestIndex;
			P.m_LastResponseStatus = Status;
		}
		lock_unlock(P.m_Lock);
	}
	if(Status == STATUS_OK)
	{
		lock_wait(pSelf->m_Lock);
		if(InfoSerial > pSelf->m_LastSuccessfulInfoSerial)
		{
			pSelf->m_LastSuccessfulInfoSerial = InfoSerial;
		}
		lock_unlock(pSelf->m_Lock);
	}
	else if(Status == STATUS_NEEDINFO)
	{
		lock_wait(pSelf->m_Lock);
		if(InfoSerial == pSelf->m_LastSuccessfulInfoSerial)
		{
			// Tell other requests that they need to send the info again.
			pSelf->m_LastSuccessfulInfoSerial -= 1;
		}
		lock_unlock(pSelf->m_Lock);
	}
	return 0;
}

void CRegister::SendDelete(int Protocol, bool Shutdown)
{
	CProtocol &P = m_aProtocols[Protocol];
	{
		lock_wait(P.m_Lock);
		const bool ShouldSendDelete = P.m_LastResponseStatus == STATUS_OK;
		P.m_LastResponseStatus = STATUS_NONE;
		lock_unlock(P.m_Lock);
		if(!ShouldSendDelete)
			return;
	}

	char aAddress[64];
	str_format(aAddress, sizeof(aAddress), "%sconnecting-address.invalid:%d", ProtocolToScheme(Protocol), m_ServerPort);

	char aSecret[UUID_MAXSTRSIZE];
	format_uuid(m_Secret, aSecret, sizeof(aSecret));

	CHttpRequest Request("POST", Config()->m_SvRegisterUrl, Shutdown ? SHUTDOWN_DELETE_TIMEOUT : DELETE_TIMEOUT, Protocol == PROTOCOL_IPV4 ? HTTP_IPRESOLVE_IPV4ONLY : HTTP_IPRESOLVE_IPV6ONLY);
	Request.AddHeader("Action: delete");

	char aHeader[256];
	str_format(aHeader, sizeof(aHeader), "Address: %s", aAddress);
	Request.AddHeader(aHeader);

	str_format(aHeader, sizeof(aHeader), "Secret: %s", aSecret);
	Request.AddHeader(aHeader);

	lock_wait(m_Lock);
	for(int i = 0; i < m_NumExtraHeaders; i++)
		Request.AddHeader(m_aaExtraHeaders[i]);
	lock_unlock(m_Lock);

	Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, ProtocolToSystem(Protocol), "deleting...");

	Request.StartRunBlocking();
	if(Request.Result() != 0)
		Console()->Print(IConsole::OUTPUT_LEVEL_ADDINFO, ProtocolToSystem(Protocol), "error sending delete request to master");
}

void CRegister::OnToken(int Protocol, const char *pToken)
{
	CProtocol &P = m_aProtocols[Protocol];
	lock_wait(P.m_Lock);
	P.m_NewChallengeToken = true;
	P.m_HaveChallengeToken = true;
	str_copy(P.m_aChallengeToken, pToken, sizeof(P.m_aChallengeToken));
	P.m_NextRegister = time_get();
	lock_unlock(P.m_Lock);
}

CRegister::CRegister()
{
	m_pEngine = 0;
	m_pConfig = 0;
	m_pConsole = 0;

	m_GotServerInfo = false;
	m_NumExtraHeaders = 0;
	m_aCommunityToken[0] = '\0';
	m_aConnlessTokenHex[0] = '\0';
	m_aParsedExtra[0] = '\0';
	m_aParsedRegister[0] = '\0';
	m_aRegisterEnabled[0] = false;
	m_aRegisterEnabled[1] = false;

	m_Secret = random_uuid();
	m_ChallengeSecret = random_uuid();

	for(int i = 0; i < NUM_PROTOCOLS; i++)
	{
		m_aProtocols[i].m_Context.m_pParent = this;
		m_aProtocols[i].m_Context.m_Protocol = i;
		m_aProtocols[i].m_Enabled = false;
		m_aProtocols[i].m_NewChallengeToken = false;
		m_aProtocols[i].m_HaveChallengeToken = false;
		m_aProtocols[i].m_aChallengeToken[0] = '\0';
		m_aProtocols[i].m_PrevRegister = -1;
		m_aProtocols[i].m_NumTotalRequests = 0;
		m_aProtocols[i].m_LastResponseStatus = STATUS_NONE;
		m_aProtocols[i].m_LastResponseIndex = -1;
		m_aProtocols[i].m_NextRegister = -1;
		m_aProtocols[i].m_Lock = lock_create();
	}
	m_Lock = lock_create();
	m_InfoSerial = -1;
	m_LastSuccessfulInfoSerial = -1;
	m_ServerPort = 0;

	static const int HEADER_LEN = sizeof(SERVERBROWSE_CHALLENGE);
	mem_copy(m_aVerifyPacketPrefix, SERVERBROWSE_CHALLENGE, HEADER_LEN);
	format_uuid(m_ChallengeSecret, m_aVerifyPacketPrefix + HEADER_LEN, sizeof(m_aVerifyPacketPrefix) - HEADER_LEN);
	m_aVerifyPacketPrefix[HEADER_LEN + UUID_MAXSTRSIZE - 1] = ':';
}

CRegister::~CRegister()
{
	OnShutdown();

	for(int i = 0; i < NUM_PROTOCOLS; i++)
	{
		lock_destroy(m_aProtocols[i].m_Lock);
	}
	lock_destroy(m_Lock);
}

bool CRegister::StatusFromString(int *pResult, const char *pString)
{
	if(str_comp(pString, "success") == 0)
	{
		*pResult = STATUS_OK;
	}
	else if(str_comp(pString, "need_challenge") == 0)
	{
		*pResult = STATUS_NEEDCHALLENGE;
	}
	else if(str_comp(pString, "need_info") == 0)
	{
		*pResult = STATUS_NEEDINFO;
	}
	else if(str_comp(pString, "error") == 0)
	{
		*pResult = STATUS_ERROR;
	}
	else
	{
		*pResult = -1;
		return true;
	}
	return false;
}

bool CRegister::ProtocolFromString(int *pResult, const char *pString)
{
	if(str_comp(pString, "tw0.7/ipv6") == 0)
	{
		*pResult = PROTOCOL_IPV6;
	}
	else if(str_comp(pString, "tw0.7/ipv4") == 0)
	{
		*pResult = PROTOCOL_IPV4;
	}
	else
	{
		*pResult = -1;
		return true;
	}
	return false;
}

const char *CRegister::ProtocolToScheme(int Protocol)
{
	switch(Protocol)
	{
		case PROTOCOL_IPV4: return "tw-0.7+udp://";
		case PROTOCOL_IPV6: return "tw-0.7+udp://";
	}
	return "invalid protocol";
}

const char *CRegister::ProtocolToString(int Protocol)
{
	switch(Protocol)
	{
		case PROTOCOL_IPV4: return "tw0.7/ipv4";
		case PROTOCOL_IPV6: return "tw0.7/ipv6";
	}
	return "invalid protocol";
}

const char *CRegister::ProtocolToSystem(int Protocol)
{
	switch(Protocol)
	{
		case PROTOCOL_IPV4: return "register/ipv4";
		case PROTOCOL_IPV6: return "register/ipv6";
	}
	return "invalid protocol";
}

void CRegister::Init(IEngine *pEngine, CConfig *pConfig, IConsole *pConsole, TOKEN SecurityToken)
{
	m_pEngine = pEngine;
	m_pConfig = pConfig;
	m_pConsole = pConsole;
	m_ServerPort = m_pConfig->m_SvRegisterPort > 0 ? m_pConfig->m_SvRegisterPort : m_pConfig->m_SvPort;

	lock_wait(m_Lock);
	str_format(m_aConnlessTokenHex, sizeof(m_aConnlessTokenHex), "%08x", SecurityToken);
	lock_unlock(m_Lock);
}

void CRegister::UpdateConfig()
{
	lock_wait(m_Lock);
	str_copy(m_aCommunityToken, Config()->m_SvRegisterCommunityToken, sizeof(m_aCommunityToken));
	// Only parse on change, so a malformed setting warns once instead of on
	// every tick.
	const char *pExtra = Config()->m_SvRegisterExtra;
	if(str_comp(m_aParsedExtra, pExtra) != 0)
	{
		str_copy(m_aParsedExtra, pExtra, sizeof(m_aParsedExtra));
		m_NumExtraHeaders = 0;
		char aHeader[128];
		while(pExtra)
		{
			pExtra = NextToken(pExtra, aHeader, sizeof(aHeader));
			if(!aHeader[0])
				continue;
			if(m_NumExtraHeaders == MAX_EXTRA_HEADERS)
			{
				Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "register", "too many extra headers, ignoring the rest");
				break;
			}
			if(!str_find(aHeader, ": "))
			{
				Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "register", "extra header must contain ': ', ignoring");
				continue;
			}
			str_copy(m_aaExtraHeaders[m_NumExtraHeaders], aHeader, sizeof(m_aaExtraHeaders[0]));
			m_NumExtraHeaders++;
		}
	}
	lock_unlock(m_Lock);
}

void CRegister::UpdateProtocols(int Nettype)
{
	const char *pValue = Config()->m_SvRegister;
	if(str_comp(m_aParsedRegister, pValue) != 0)
	{
		str_copy(m_aParsedRegister, pValue, sizeof(m_aParsedRegister));
		ParseRegisterConfig();
	}

	bool aEnabled[NUM_PROTOCOLS] = {m_aRegisterEnabled[PROTOCOL_IPV4], m_aRegisterEnabled[PROTOCOL_IPV6]};

	// Only advertise address families the server actually listens on.
	if(!(Nettype & NETTYPE_IPV4))
		aEnabled[PROTOCOL_IPV4] = false;
	if(!(Nettype & NETTYPE_IPV6))
		aEnabled[PROTOCOL_IPV6] = false;

	for(int i = 0; i < NUM_PROTOCOLS; i++)
	{
		if(aEnabled[i] == m_aProtocols[i].m_Enabled)
			continue;
		m_aProtocols[i].m_Enabled = aEnabled[i];
		if(aEnabled[i])
		{
			lock_wait(m_aProtocols[i].m_Lock);
			m_aProtocols[i].m_NextRegister = time_get();
			lock_unlock(m_aProtocols[i].m_Lock);
		}
		else
		{
			SendDelete(i, false);
		}
	}
}

void CRegister::ParseRegisterConfig()
{
	m_aRegisterEnabled[PROTOCOL_IPV4] = false;
	m_aRegisterEnabled[PROTOCOL_IPV6] = false;
	const char *pValue = Config()->m_SvRegister;
	if(str_comp(pValue, "1") == 0)
	{
		m_aRegisterEnabled[PROTOCOL_IPV4] = true;
		m_aRegisterEnabled[PROTOCOL_IPV6] = true;
	}
	else if(str_comp(pValue, "0") != 0 && pValue[0] != '\0')
	{
		const char *pIter = pValue;
		char aToken[32];
		while(pIter)
		{
			pIter = NextToken(pIter, aToken, sizeof(aToken));
			if(!aToken[0])
				continue;
			if(str_comp_nocase(aToken, "ipv4") == 0 || str_comp_nocase(aToken, "tw0.7/ipv4") == 0)
			{
				m_aRegisterEnabled[PROTOCOL_IPV4] = true;
			}
			else if(str_comp_nocase(aToken, "ipv6") == 0 || str_comp_nocase(aToken, "tw0.7/ipv6") == 0)
			{
				m_aRegisterEnabled[PROTOCOL_IPV6] = true;
			}
			else if(str_comp_nocase(aToken, "tw0.7") == 0 || str_comp_nocase(aToken, "all") == 0)
			{
				m_aRegisterEnabled[PROTOCOL_IPV4] = true;
				m_aRegisterEnabled[PROTOCOL_IPV6] = true;
			}
			else
			{
				char aBuf[64];
				str_format(aBuf, sizeof(aBuf), "unknown protocol '%s'", aToken);
				Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "register", aBuf);
			}
		}
	}
}

void CRegister::RegisterUpdate(int Nettype, TOKEN SecurityToken)
{
	// The token the master has to put into its challenge packet rotates with
	// the token seed, so refresh the advertised value on every update instead
	// of remembering the one from startup.
	lock_wait(m_Lock);
	str_format(m_aConnlessTokenHex, sizeof(m_aConnlessTokenHex), "%08x", SecurityToken);
	lock_unlock(m_Lock);

	UpdateConfig();
	UpdateProtocols(Nettype);

	if(!m_GotServerInfo)
		return;
	for(int i = 0; i < NUM_PROTOCOLS; i++)
	{
		if(m_aProtocols[i].m_Enabled)
			UpdateRegister(i);
	}
}

void CRegister::OnNewInfo(const char *pInfo)
{
	if(m_GotServerInfo && str_comp(m_aServerInfo, pInfo) == 0)
	{
		return;
	}
	m_GotServerInfo = true;
	{
		lock_wait(m_Lock);
		str_copy(m_aServerInfo, pInfo, sizeof(m_aServerInfo));
		m_InfoSerial++;
		lock_unlock(m_Lock);
	}

	// Send the new info right away instead of waiting for the next periodic
	// registration, but not more than once per second.
	const int64 Now = time_get();
	const int64 Freq = time_freq();
	for(int i = 0; i < NUM_PROTOCOLS; i++)
	{
		CProtocol &P = m_aProtocols[i];
		if(!P.m_Enabled || P.m_PrevRegister <= 0)
			continue;
		if(Now - P.m_PrevRegister >= Freq)
			P.m_NextRegister = Now;
	}
}

void CRegister::OnShutdown()
{
	for(int i = 0; i < NUM_PROTOCOLS; i++)
	{
		if(!m_aProtocols[i].m_Enabled)
			continue;
		SendDelete(i, true);
	}
}

bool CRegister::OnPacket(const CNetChunk *pPacket)
{
	if((pPacket->m_Flags & NETSENDFLAG_CONNLESS) == 0)
		return false;

	if(pPacket->m_DataSize >= (int) sizeof(m_aVerifyPacketPrefix) &&
		mem_comp(pPacket->m_pData, m_aVerifyPacketPrefix, sizeof(m_aVerifyPacketPrefix)) == 0)
	{
		CUnpacker Unpacker;
		Unpacker.Reset(pPacket->m_pData, pPacket->m_DataSize);
		Unpacker.GetRaw(sizeof(m_aVerifyPacketPrefix));
		const char *pProtocol = Unpacker.GetString(0);
		const char *pToken = Unpacker.GetString(0);
		if(Unpacker.Error())
		{
			Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "register", "got erroneous challenge packet from master");
			return true;
		}

		if(Config()->m_Debug)
			dbg_msg("register", "got challenge token, protocol='%s' token='%s'", pProtocol, pToken);

		int Protocol;
		if(ProtocolFromString(&Protocol, pProtocol))
		{
			Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "register", "got challenge packet with unknown protocol");
			return true;
		}
		OnToken(Protocol, pToken);
		return true;
	}
	return false;
}
