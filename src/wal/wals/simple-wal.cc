/**
 * This work copyright Chao Sun(qq:296449610) and licensed under
 * a Creative Commons Attribution 3.0 Unported License(https://creativecommons.org/licenses/by/3.0/).
 */

#include <memory>
#include <unistd.h>
#include <sys/mman.h>

#include "../../common/common-def.h"

#include "../../common/ientry.h"

#include "simple-wal.h"
#include "../../utils/file-utils.h"
#include "../../utils/io-utils.h"
#include "../../utils/codec-utils.h"
#include "../../utils/protobuf-utils.h"
#include "../../common/global-vars.h"
#include "../../common/buffer.h"

namespace flyingkv {
using namespace utils;
namespace wal {
SimpleWal::SimpleWal(std::string &rootDir, common::EntryCreateHandler &&entryCreateHandler) :
        m_sRootDir(rootDir), m_entryCreator(std::move(entryCreateHandler)) {
    if (rootDir.empty()) {
        LOGFFUN << "wal root dir is empty";
    }

    m_sRootDir = rootDir;
    if (-1 == utils::FileUtils::CreateDirPath(m_sRootDir.c_str(), 0775)) {
        LOGFFUN << "create wal dir " << m_sRootDir.c_str() << " failed.";
    }

    m_sLogFilePath = m_sRootDir + "/" + SMLOG_PREFIX_NAME;
}

SimpleWal::~SimpleWal() {
    if (-1 != m_fd) {
        close(m_fd);
        m_fd = -1;
    }
}

uint64_t SimpleWal::AppendEntry(common::IEntry *entry) {
    if (UNLIKELY(!m_bLoaded)) {
        LOGFFUN << "simple wal has not been loaded.";
    }
    std::shared_ptr<common::Buffer> eb;
    if (UNLIKELY(!entry->Encode(eb))) {
        LOGFFUN << "simple wal encode entry error";
    }

    auto walEntryStartOffset = m_fileSize;
    auto rawEntrySize = eb->AvailableLength();
    auto walEntrySize = rawEntrySize + SMLOG_ENTRY_EXTRA_FIELDS_SIZE;
    // mpo will be Putted in Buffer 'b'.
    auto mpo = common::g_pMemPool->Get(uint32_t(walEntrySize));
    auto bufferStart = (uchar*)(mpo->Pointer());
    common::Buffer wb;
    wb.Refresh(bufferStart, bufferStart + walEntrySize - 1, bufferStart, bufferStart + walEntrySize - 1, mpo);
    // size
    ByteOrderUtils::WriteUInt32(wb.GetPos(), uint32_t(rawEntrySize));
    wb.SetPos(wb.GetPos() + SMLOG_SIZE_LEN);

    // log id
    ByteOrderUtils::WriteUInt64(wb.GetPos(), uint32_t(m_currentLogIdx));
    wb.SetPos(wb.GetPos() + SMLOG_ENTRY_ID_LEN);

    // content
    memcpy(eb->GetPos(), wb.GetPos(), size_t(rawEntrySize));
    wb.SetPos(wb.GetPos() + rawEntrySize);

    // offset
    ByteOrderUtils::WriteUInt32(wb.GetPos(), walEntryStartOffset);

    WriteFileFullyWithFatalLOG(m_fd, (char*)(wb.GetStart()), walEntrySize, m_sLogFilePath.c_str());
    FDataSyncFileWithFatalLOG(m_fd, m_sLogFilePath.c_str());
    m_fileSize += walEntrySize;
    auto entryId = m_currentLogIdx;
    m_mpEntriesIdEndOffset[m_currentLogIdx] = m_fileSize - 1;
    increase_log_id();
    return entryId;
}

// TODO(sunchao): 1. 如果以后支持大量的log，就要把这个接口改成batch load
//                2. 添加log idx支持跳过checkpoint已经apply的log。不过目前这样没有bug，因为kv engine这几个操作都是幂等的。
std::vector<WalEntry> SimpleWal::Load() {
    std::vector<WalEntry> rs;
    if (!utils::FileUtils::Exist(m_sLogFilePath)) {
        LOGDFUN2("create sm log file ", m_sLogFilePath.c_str());
        if (-1 == (m_fd = utils::FileUtils::Open(m_sLogFilePath, O_WRONLY, O_CREAT, 0644))) {
            auto err = errno;
            LOGFFUN << "create sm log file " << m_sLogFilePath.c_str() << " failed with errmsg " << strerror(err);
        }

        // write file header.
        uchar header[SMLOG_MAGIC_NO_LEN];
        ByteOrderUtils::WriteUInt32(header, SMLOG_MAGIC_NO);
        WriteFileFullyWithFatalLOG(m_fd, (char*)header, SMLOG_MAGIC_NO_LEN, m_sLogFilePath.c_str());
    } else {
        if (-1 == (m_fd = utils::FileUtils::Open(m_sLogFilePath, O_RDWR, 0, 0))) {
            auto err = errno;
            LOGFFUN << "open sm log file " << m_sLogFilePath.c_str() << " failed with errmsg " << strerror(err);
        }

        // 1. check if file is empty.
        auto fileSize = utils::FileUtils::GetFileSize(m_fd);
        if (-1 == fileSize) {
            auto err = errno;
            LOGFFUN << "get file size for " << m_sLogFilePath.c_str() << " failed with errmsg " << strerror(err);
        }
        if (0 == (m_fileSize = (uint32_t)fileSize)) {
            LOGDFUN3("sm log file ", m_sLogFilePath.c_str(), " is empty!");
            return std::vector<WalEntry>();
        }

        // 2. check if file header is corrupt.
        if (m_fileSize < SMLOG_MAGIC_NO_LEN) {
            LOGFFUN << "sm log file " << m_sLogFilePath.c_str() << " header is corrupt!";
        }

        // TODO(sunchao): 如果以后扩展这个wal，支持大量的logs，就不要mmap了
        auto mapRet = mmap(nullptr, (size_t)m_fileSize, PROT_READ, MAP_PRIVATE, m_fd, 0);
        if (MAP_FAILED == mapRet) {
            auto err = errno;
            LOGFFUN << "mmap sm log " << m_sLogFilePath.c_str() << " failed with errmsg " << strerror(err);
        }

        auto buf = (uchar*)mapRet;
        uint32_t offset = 0;
        uint32_t magicNo = ByteOrderUtils::ReadUInt32(buf);
        if (SMLOG_MAGIC_NO != magicNo) {
            LOGFFUN << "sm log " << m_sLogFilePath.c_str() << " header is corrupt!";
        }

        offset += SMLOG_MAGIC_NO_LEN;
        common::Buffer b;
        // 3. load sm log content
        while (offset <= m_fileSize - 1) {
            // 3.1 read sm log len
            auto len = ByteOrderUtils::ReadUInt32(buf + offset);
            auto startPos = ByteOrderUtils::ReadUInt32(buf + offset + SMLOG_ENTRY_ID_LEN + len);
            if (startPos != offset) {
                LOGFFUN << "parse sm log " << m_sLogFilePath.c_str() << " failed at offset " << offset;
            }

            auto entryId = ByteOrderUtils::ReadUInt64(buf + offset + SMLOG_SIZE_LEN);
            auto startPtr = buf + offset + SMLOG_SIZE_LEN + SMLOG_ENTRY_ID_LEN;
            auto endPtr = buf + len - 1;
            b.Refresh(startPtr, endPtr, startPtr, endPtr, nullptr);
            auto entry = m_entryCreator();
            if (!entry->Decode(b)) {
                LOGFFUN << "deserialize sm log entry at offset " << offset << " failed!";
            }

            auto we = WalEntry(entryId, entry);
            rs.push_back(std::move(we));
            offset += (len + SMLOG_ENTRY_EXTRA_FIELDS_SIZE);
            m_mpEntriesIdEndOffset[entryId] = offset - 1;
        }

        if (-1 == munmap(mapRet, m_fileSize)) {
            auto err = errno;
            LOGFFUN << "munmap file " << m_sLogFilePath.c_str() << " failed with errmsg " << strerror(err);
        }
    }

    LSeekFileWithFatalLOG(m_fd, 0, SEEK_END, m_sLogFilePath.c_str());
    return rs;
}

bool SimpleWal::TruncateAhead(uint64_t id) {
    if (UNLIKELY(!m_bLoaded)) {
        LOGFFUN << "simple wal has not been loaded.";
    }

    if (m_mpEntriesIdEndOffset.end() == m_mpEntriesIdEndOffset.find(id)) {
        return false;
    }

    if (0 == m_fileSize) {
        return true;
    }

    auto endOffset = m_mpEntriesIdEndOffset[id];
    auto offset = endOffset + 1;
    if (offset == m_fileSize) {
        if (-1 == ftruncate(m_fd, SMLOG_MAGIC_NO_LEN/*reserve header*/)) {
            LOGFFUN << "simple wal clean failed with errmsg " << strerror(errno);
        }
        m_fileSize = 0;
        m_mpEntriesIdEndOffset.clear();

        return true;
    }

    auto leftBufLen = m_fileSize - offset;
    auto buf = new char[leftBufLen];
    LSeekFileWithFatalLOG(m_fd, offset, SEEK_SET, m_sLogFilePath.c_str());
    if (-1 == utils::IOUtils::ReadFully_V2(m_fd, &buf, size_t(leftBufLen))) {
        LOGFFUN << "simple wal " << m_sLogFilePath.c_str() << " read error " << strerror(errno);
    }

    LSeekFileWithFatalLOG(m_fd, SMLOG_MAGIC_NO_LEN, SEEK_SET, m_sLogFilePath.c_str());
    // TODO(sunchao): 以下俩文件操作不是原子的，如果过程中挂了，会导致不一致。先这样，有时间加个meta并且通过临时文件重命名的方式来做。
    if (-1 == utils::IOUtils::WriteFully(m_fd, buf, size_t(leftBufLen))) {
        LOGFFUN << "simple wal " << m_sLogFilePath.c_str() << " write error " << strerror(errno);
    }

    if (-1 == ftruncate(m_fd, 0)) {
        LOGFFUN << "simple wal truncate ahead failed with errmsg " << strerror(errno);
    }

    m_fileSize = uint32_t(SMLOG_MAGIC_NO_LEN + leftBufLen);
    // TODO(sunchao): 此处需要优化，有两点：1. id无限增长的话会导致溢出，可以通过多文件的方式解决。
    //                                   2. 每次truncate都需要从0开始清理这个map不好，需要记录last清理id
    for (uint32_t i = 0; i <= id; ++i) {
        m_mpEntriesIdEndOffset.erase(i);
    }

    return true;
}

void SimpleWal::Reset() {
    if (-1 == ftruncate(m_fd, 0)) {
        LOGFFUN << "simple wal clean failed with errmsg " << strerror(errno);
    }
    m_currentLogIdx = 0;
    m_fileSize = 0;
    m_mpEntriesIdEndOffset.clear();
}
}
}
