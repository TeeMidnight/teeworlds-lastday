/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#include <base/system.h>
#include <engine/engine.h>
#include <game/server/database/database.h>
#include <game/server/database/database_postgres.h>
#include <game/server/database/database_sqlite.h>
#include <game/server/database/database_util.h>

CDatabase::~CDatabase()
{
	// The owner normally drained already; this only catches a caller that
	// forgot, so a queued job never outlives the connection it uses.
	DrainJobs();
	lock_destroy(m_Lock);
}

void CDatabase::QueueJob(CDbJob *pJob)
{
	pJob->m_pDB = this;

	if(!m_pEngine)
	{
		// no pool (e.g. a test harness): stay correct, just blocking
		RunDbJob(pJob);
		delete pJob;
		return;
	}

	m_lDbJobs.add(pJob);
	DispatchJob();
}

void CDatabase::DispatchJob()
{
	if(m_lDbJobs.size() == 0)
		return;

	// Only the front job may run; the rest wait for it, which keeps the writes
	// of one player, and of a world and its shutdown flush, in submission order.
	CDbJob *pJob = m_lDbJobs[0];
	if(pJob->m_Dispatched)
		return;
	pJob->m_Dispatched = true;
	m_pEngine->AddJob(&pJob->m_Job, RunDbJob, pJob);
}

void CDatabase::ReapJobs()
{
	// FIFO: once the front job finished, free it and start the next
	while(m_lDbJobs.size() && m_lDbJobs[0]->m_Dispatched && m_lDbJobs[0]->m_Job.Status() == CJob::STATE_DONE)
	{
		delete m_lDbJobs[0];
		m_lDbJobs.remove_index(0);
		DispatchJob();
	}
}

bool CDatabase::DrainJobs()
{
	const int64 Start = time_get();
	while(m_lDbJobs.size())
	{
		ReapJobs();
		if(!m_lDbJobs.size())
			break;
		if(time_get() - Start > (int64) time_freq() * 5)
		{
			dbg_msg("database", "%d database job(s) still running, leaving the connection open", m_lDbJobs.size());
			return false;
		}
		thread_sleep(1);
	}
	return true;
}

void CDatabase::QueueWorldSave(const char *pMap, const char *pJson, bool HasJson)
{
	if(!pMap || !pMap[0])
		return;

	CDbJob *pJob = new CDbJob();
	pJob->m_Type = CDbJob::TYPE_WORLD;
	str_copy(pJob->m_aMapName, pMap, sizeof(pJob->m_aMapName));
	pJob->m_HasJson = HasJson;
	if(HasJson && pJson)
	{
		const int Len = str_length(pJson);
		pJob->m_Json.set_size(Len + 1);
		mem_copy(pJob->m_Json.base_ptr(), pJson, Len + 1);
	}
	QueueJob(pJob);
}

CDatabase *CreateDatabase(const CDatabase::SConfig &Config)
{
	const char *pBackend = Config.m_pBackend && Config.m_pBackend[0] ? Config.m_pBackend : "sqlite";

	if(str_comp_nocase(pBackend, "sqlite") == 0)
		return new CDatabaseSQLite(Config);

	if(str_comp_nocase(pBackend, "postgres") == 0)
		return new CDatabasePostgres(Config);

	dbg_msg("database", "unknown database backend '%s'", pBackend);
	return 0;
}
