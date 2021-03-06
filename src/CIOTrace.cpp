/*
 * CIOTrace.cpp
 *
 *  Created on: Jun 1, 2019
 *      Author: avner
 */

#include <string>
#include <memory>
#include <string.h>
#include <sstream>
#include "CIOTrace.h"
#include "CGetProcessInfo.h"
#include "constants.h"
#include "CStraceOutputParser.h"
using namespace std;


CIOTrace::CIOTrace(bool reportOnline, bool incomplete, bool followFork, tProcessId attachProcessId, const std::string command) :
	m_bReportOnline(reportOnline),
	m_bIncludeIncomplete(incomplete),
	m_bFollowFork(followFork),
	m_nAttachProcess(attachProcessId),
	m_sCommand(command)
{
	// TODO Auto-generated constructor stub

}

CIOTrace::~CIOTrace() {
	// TODO Auto-generated destructor stub
}
void CIOTrace::printReport(){
	LOG(eReport)<< "Filename, Read bytes, Written bytes, Opened, Read op, Write op"<<endl;

	tFileInfoMap::iterator l_oIter=m_oFilesInfoMap.begin();
	while (l_oIter!=m_oFilesInfoMap.end())
	{
		LOG(eReport)<<
			l_oIter->first.c_str()<<CSV_SEP<<
			l_oIter->second.getRead()<<CSV_SEP<<
			l_oIter->second.getWritten()<<CSV_SEP<<
			l_oIter->second.getOpened()<<CSV_SEP<<
			l_oIter->second.getReadOp()<<CSV_SEP<<
			l_oIter->second.getWriteOp()<<endl;
		++l_oIter;
	}
}

bool CIOTrace::monitor()
{
    int status;
    char l_sLine[LINE_MAX_BUFF];

    const char *TRACE_CMD= "strace -o \\!cat -e read,write,open,openat,close ";

    string l_sStraceCommand= string(TRACE_CMD) + string(m_bFollowFork ? " -f  " : "") +
        (m_nAttachProcess!=0 ? string("-p ") + to_string(m_nAttachProcess) : m_sCommand);

    LOG(eInfo) << l_sStraceCommand << endl;
    FILE*  fp = popen(l_sStraceCommand.c_str() , "r");
    if (fp == NULL)
    {
        LOG(eFatal) << "Failed to execute strace, make sure strace is installed" << endl;
        return EXIT_ERROR;
    }

    while (fgets(l_sLine, LINE_MAX_BUFF, fp) != NULL)
    {
    	processLine(l_sLine);
    }

    status = pclose(fp);
    if (status == -1) {
        LOG(eFatal) << "Failed to close strace process" << endl;
        return false;
    }
    return true;

}

void CIOTrace::addProcessFilesAndSocketsToActiveFiles(tProcessId a_nPid)
{
	vector<CGetProcessInfo::tFile> activeFiles=  CGetProcessInfo::getProcessOpenFiles(a_nPid);
	for (CGetProcessInfo::tFile file : activeFiles)
		AddActiveFile(a_nPid, file.m_nFileNum, file.m_sFileName, false);
}

const char *g_saStdPrefixes[] =
{
    "stdin_",
    "stdout_",
    "stderr_"
};

void CIOTrace::RemoveActiveFile(tProcessId a_nPid, tFileNum a_nFileNum)
{
	if (!m_oActiveFilesMap.erase(CActiveFileId(a_nPid, a_nFileNum)))
	{
		LOG(eError)<< "Didn't find file id "<< a_nFileNum <<endl;
	}
	else
	{
		LOG(eInfo)<< "file id removed from active files maps" << a_nFileNum <<endl;
	}
}

void CIOTrace::AddActiveFile(tProcessId a_nPid, tFileNum a_nFileNum, const std::string & a_sFileName, bool a_bOpenOp)
{
    string l_sFullName;
    if (a_nFileNum < 3)
        l_sFullName = string(g_saStdPrefixes[a_nFileNum]) + a_sFileName;
    else
        l_sFullName = a_sFileName;

    tActiveFiles::iterator l_oIter=m_oActiveFilesMap.find(CActiveFileId(a_nPid,a_nFileNum));
    if (l_oIter!=m_oActiveFilesMap.end())
    {
        LOG(eFatal) << "Trying to add file while file num already open pid=" << a_nPid << " desc id=" <<a_nFileNum << " replacing with "<< a_sFileName << endl;
        RemoveActiveFile(a_nPid, a_nFileNum);
    }
    LOG(eInfo) << "added file " << l_sFullName << " desc id="<< a_nFileNum << " pid="<<a_nPid<< endl;
    pair<tActiveFiles::iterator, bool> l_oIterNewActiveFile;
    l_oIterNewActiveFile=m_oActiveFilesMap.insert(tActiveFiles::value_type(CActiveFileId(a_nPid, a_nFileNum), CActiveFileInfo(l_sFullName)));

    tFileInfoMap::iterator l_oFileInfoIter = m_oFilesInfoMap.find(l_sFullName);
    if (l_oFileInfoIter==m_oFilesInfoMap.end())
    {
        pair<tFileInfoMap::iterator, bool> l_oFileInfoIter;
        l_oFileInfoIter=m_oFilesInfoMap.insert(
            tFileInfoMap::value_type(l_sFullName, CFileInfo()));
        l_oIterNewActiveFile.first->second.setFileInfoIter(l_oFileInfoIter.first);
    }
    else
    {
        if (a_bOpenOp)
            l_oFileInfoIter->second.open();
        l_oIterNewActiveFile.first->second.setFileInfoIter(l_oFileInfoIter);
    }

}

string CIOTrace::genIncompleteFilenameName(tProcessId a_nPid, tFileNum a_nFileID)
{
    ostringstream commandStream;

    switch (a_nFileID)
    {
        case 0:
        	commandStream << "descriptor_pid_"<<a_nPid<<"_stdin_desc_id_"<<a_nFileID;
            break;
        case 1:
        	commandStream << "descriptor_pid_"<<a_nPid<<"_stdout_desc_id_"<<a_nFileID;
            break;
        case 2:
        	commandStream << "descriptor_pid_"<<a_nPid<<"_stderr_desc_id_"<<a_nFileID;
            break;
        default:
            addProcessFilesAndSocketsToActiveFiles(a_nPid);
            CActiveFileInfo *l_opFile=GetActiveFile(a_nPid, a_nFileID);
            // In case the file was added to active list, we return empty string
            if (!l_opFile)
            	commandStream <<"descriptor_pid_"<< a_nPid << "_desc_id_" << a_nFileID;
    };
    return commandStream.str();

}


void CIOTrace::processLine(char *a_sLine)
{
	std::unique_ptr<CStraceOutputParser::CStraceOperation> l_oStraceOp=CStraceOutputParser::parseStraceLine(a_sLine, m_bFollowFork);
	// Not a strace output line (this can happen since the command line we asked strace to run writes to same pipe)
	if (!l_oStraceOp)
	{
		// We print non strace lines which may have come from the command line we asked strace to run
		LOG(ePrint)<< a_sLine<< endl;
		return;
	}
    // In case the flag -f wasn't added to strace, the pid for each line isn't reported
    // so we need to use the pid we got for the tracing command
	if (!m_bFollowFork)
		l_oStraceOp->setPid(m_nAttachProcess);

    if (l_oStraceOp->pid()>0)
    {
        tPidSet::iterator l_oIter= m_oPidSet.find(l_oStraceOp->pid());
        if (l_oIter==m_oPidSet.end())
        {
            string l_sProcName=CGetProcessInfo::getCommandLine(l_oStraceOp->pid());
            LOG(ePrint)<< "new pid "<<l_oStraceOp->pid()<<" process "<< l_sProcName;
            // Add the files already opened into the active files list
            m_oPidSet.insert(tPidSet::value_type(l_oStraceOp->pid()));
            addProcessFilesAndSocketsToActiveFiles(l_oStraceOp->pid());
        }
        else
        {
            LOG(eInfo) << "Handling pid="<< l_oStraceOp->pid()<<endl;
        }
    }

    if (l_oStraceOp->resumed())
		handleUnfinished(*l_oStraceOp);
    else
    {
		switch (l_oStraceOp->opType())
		{
			case eOpen: // open
				handleOpen(dynamic_cast<CStraceOutputParser::CStraceOpenOperation&>(*l_oStraceOp));
				return;
				break;
			case eRead: // read
				handleRead(dynamic_cast<CStraceOutputParser::CStraceReadOperation&>(*l_oStraceOp));
				return;
				break;
			case eWrite: // write
				handleWrite(dynamic_cast<CStraceOutputParser::CStraceWriteOperation&>(*l_oStraceOp));
				return;
				break;
			case eClose: // close
				handleClose(dynamic_cast<CStraceOutputParser::CStraceCloseOperation&>(*l_oStraceOp));
				return;
				break;
		}
    }
}


void CIOTrace::addPendingIoOperation(
		tProcessId            	a_nPid,
		tIoOp     				a_nOp,
		tFileNum                a_nFileId,
		std::string             a_sFilename)
{
	switch (a_nOp)
	{
	case tIoOp::eRead:
	case tIoOp::eWrite:
	case tIoOp::eClose:
		m_oPedningIoOperations.insert(
			tFilePendingInfoMap::value_type(a_nPid, CPendingIoOp(a_nOp, a_nFileId)));
		LOG(eInfo)<< "Added incomplete operation data pid="<< a_nPid << ", op=" << a_nOp << "fileid=" << a_nFileId<<endl;
		break;
	case tIoOp::eOpen:
		m_oPedningIoOperations.insert(
			tFilePendingInfoMap::value_type(a_nPid, CPendingIoOp(a_nOp, a_sFilename)));
		LOG(eInfo) << "Added incomplete operation data pid="<< a_nPid << ", op=" << a_nOp << "file name=" << a_sFilename<<endl;
	   break;
	}
}

CActiveFileInfo *CIOTrace::GetActiveFile(tProcessId a_nPid, tFileNum a_nFileNum)
{
	tActiveFiles::iterator l_oIter;
	l_oIter=m_oActiveFilesMap.find(CActiveFileId(a_nPid, a_nFileNum));
	if (l_oIter==m_oActiveFilesMap.end())
		return NULL;
	else
		return &(l_oIter->second);
}

void CIOTrace::handleOpen(const CStraceOutputParser::CStraceOpenOperation & a_oStraceOp)
{
	if (a_oStraceOp.unfinished())
	{
		addPendingIoOperation(a_oStraceOp.pid(), eOpen, UNKNOWN_NUM, a_oStraceOp.filename());
		return;
	}
	else
	{
		AddActiveFile(a_oStraceOp.pid(), a_oStraceOp.fileNum(), a_oStraceOp.filename());
		CActiveFileInfo *l_oFile=GetActiveFile(a_oStraceOp.pid(), a_oStraceOp.fileNum());
		if (l_oFile) l_oFile->open();
	}
}

void CIOTrace::UpdateRead(CActiveFileInfo *a_opActiveFile, const CStraceOutputParser::CStraceReadOperation & a_oStraceOp)
{
	if (m_bReportOnline)
		LOG(ePrint)<<a_opActiveFile->getName()<<CSV_SEP<<a_oStraceOp.bytes()<<CSV_SEP<<0<<endl;
	a_opActiveFile->read(a_oStraceOp.bytes());
}

void CIOTrace::handleRead(const CStraceOutputParser::CStraceReadOperation & a_oStraceOp)
{
	if (a_oStraceOp.unfinished())
	{
		addPendingIoOperation(a_oStraceOp.pid(), eRead, a_oStraceOp.fileNum(), "");
		return;
	}
	else
	{
		CActiveFileInfo *l_opActiveFile=GetActiveFile(a_oStraceOp.pid(), a_oStraceOp.fileNum());
		if (l_opActiveFile)
		{
			UpdateRead(l_opActiveFile, a_oStraceOp);
		}
		else
		{
			if (m_bIncludeIncomplete)
			{
				string l_sIncompleteName = genIncompleteFilenameName(a_oStraceOp.pid(), a_oStraceOp.fileNum());

				// If the name returns empty it means that the file name was found already and added
				// to the active file list
				if (l_sIncompleteName!="")
					AddActiveFile(a_oStraceOp.pid(), a_oStraceOp.fileNum(), l_sIncompleteName.c_str());
				CActiveFileInfo *l_opActiveFile=GetActiveFile(a_oStraceOp.pid(), a_oStraceOp.fileNum());
				if (l_opActiveFile)
				{
					UpdateRead(l_opActiveFile, a_oStraceOp);
				}
				else
				{
					LOG(eError) << "couldn't find file read details for " << a_oStraceOp.fileNum()<< endl;
				}
			}
			else {
				LOG(eError)<< "couldn't find file read details for "<< a_oStraceOp.fileNum() <<endl;
			}
		}
	}
}

void CIOTrace::updateWrite(CActiveFileInfo *a_opActiveFile, const CStraceOutputParser::CStraceWriteOperation & a_oStraceOp)
{
	if (m_bReportOnline)
		LOG(ePrint)<<a_opActiveFile->getName()<<CSV_SEP<<0<<a_oStraceOp.bytes()<<CSV_SEP<<endl;

	a_opActiveFile->write(a_oStraceOp.bytes());
}

void CIOTrace::handleWrite(const CStraceOutputParser::CStraceWriteOperation & a_oStraceOp)
{
	if (a_oStraceOp.unfinished())
	{
		addPendingIoOperation(a_oStraceOp.pid(), eWrite, a_oStraceOp.fileNum(), string(""));
		return;
	}
	else
	{
		CActiveFileInfo *l_opActiveFile=GetActiveFile(a_oStraceOp.pid(), a_oStraceOp.fileNum());
		if (l_opActiveFile)
		{
			updateWrite(l_opActiveFile, a_oStraceOp);
		}
		else
		{
			if (m_bIncludeIncomplete)
			{
				string l_sIncompleteName = genIncompleteFilenameName(a_oStraceOp.pid(), a_oStraceOp.fileNum());

				// If the name returns empty it means that the file name was found already and added
				// to the active file list
				if (l_sIncompleteName!="")
					AddActiveFile(a_oStraceOp.pid(), a_oStraceOp.fileNum(), l_sIncompleteName.c_str());

				LOG(eInfo) << "Added active file for descriptor " << a_oStraceOp.fileNum() << endl;
				CActiveFileInfo *l_opActiveFile=GetActiveFile(a_oStraceOp.pid(), a_oStraceOp.fileNum());
				if (l_opActiveFile)
				{
					updateWrite(l_opActiveFile, a_oStraceOp);
				}
				else
					LOG(eError)<< "coultn't find file write details, file num "<< a_oStraceOp.fileNum() <<endl;
			}
			else
				LOG(eError) << "couldn't find file write details, file num  " << a_oStraceOp.fileNum() << endl;
		}
	}
}
void CIOTrace::handleClose(const CStraceOutputParser::CStraceCloseOperation & a_oStraceOp)
{
	if (a_oStraceOp.unfinished())
	{
		addPendingIoOperation(a_oStraceOp.pid(), eClose, a_oStraceOp.fileNum(), "");
	}
	else
	{
		CActiveFileInfo *l_opActiveFile=GetActiveFile(a_oStraceOp.pid(), a_oStraceOp.fileNum());
		if (l_opActiveFile)
		{
			LOG(eInfo) << "closing active file ["<< l_opActiveFile->getName()<<"]"<<endl;
			RemoveActiveFile(a_oStraceOp.pid(), a_oStraceOp.fileNum());
		}
		else
			LOG(eError)<< "closing an unknown active file " <<a_oStraceOp.fileNum() << endl;
	}
}

void CIOTrace::handleUnfinished(CStraceOutputParser::CStraceOperation & a_opStraceOp)
{
	tFilePendingInfoMap::iterator l_oIter;
	l_oIter=m_oPedningIoOperations.find(a_opStraceOp.pid());
	if (l_oIter==m_oPedningIoOperations.end())
	{
		LOG(eFatal) << "couldn't find pending operation data" << endl;
		return;
	}
	CPendingIoOp &l_oPendOp = l_oIter->second;
	switch (l_oPendOp.op())
	{
		case eOpen :
			{
				CStraceOutputParser::CStraceOpenOperation l_oOp=dynamic_cast<CStraceOutputParser::CStraceOpenOperation&>(a_opStraceOp);
				l_oOp.setFilename(l_oPendOp.filename());
				l_oOp.setComplete();
				handleOpen(l_oOp);
			}
			break;
		case eClose:
			{
				CStraceOutputParser::CStraceCloseOperation & l_oOp=dynamic_cast<CStraceOutputParser::CStraceCloseOperation&>(a_opStraceOp);
				l_oOp.setFileNum(l_oPendOp.fileId());
				l_oOp.setComplete();
				handleClose(l_oOp);
			}
			break;
		case eRead:
			{
				CStraceOutputParser::CStraceReadOperation & l_oOp=dynamic_cast<CStraceOutputParser::CStraceReadOperation&>(a_opStraceOp);
				l_oOp.setFileNum(l_oPendOp.fileId());
				l_oOp.setComplete();
				handleRead(l_oOp);
			}
			break;
		case eWrite:
			{
				CStraceOutputParser::CStraceWriteOperation & l_oOp=dynamic_cast<CStraceOutputParser::CStraceWriteOperation&>(a_opStraceOp);
				l_oOp.setFileNum(l_oPendOp.fileId());
				l_oOp.setComplete();
				handleWrite(l_oOp);
			}
			break;
	}
	m_oPedningIoOperations.erase(l_oIter);
}

