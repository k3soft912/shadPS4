// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/logging/log.h"
#include "common/path_util.h"
#include "trp.h"

TRP::TRP() = default;
TRP::~TRP() = default;

void TRP::GetNPcommID(const std::filesystem::path& trophyPath, int index) {
    std::filesystem::path trpPath = trophyPath / "sce_sys/npbind.dat";
    Common::FS::IOFile npbindFile(trpPath, Common::FS::FileAccessMode::Read);
    if (!npbindFile.IsOpen()) {
        return;
    }
    if (!npbindFile.Seek(0x84 + (index * 0x180))) {
        LOG_CRITICAL(Common_Filesystem, "Failed to seek to NPbind offset");
        return;
    }
    npbindFile.ReadRaw<u8>(np_comm_id.data(), 12);
    std::fill(np_comm_id.begin() + 12, np_comm_id.end(), 0); // fill with 0, we need 16 bytes.
}

static void removePadding(std::vector<u8>& vec) {
    for (auto it = vec.rbegin(); it != vec.rend(); ++it) {
        if (*it == '>') {
            size_t pos = std::distance(vec.begin(), it.base());
            vec.resize(pos);
            break;
        }
    }
}

bool TRP::Extract(const std::filesystem::path& trophyPath) {
    std::filesystem::path title = trophyPath.filename();
    std::filesystem::path gameSysDir = trophyPath / "sce_sys/trophy/";
    if (!std::filesystem::exists(gameSysDir)) {
        return false;
    }
    for (int index = 0; const auto& it : std::filesystem::directory_iterator(gameSysDir)) {
        if (it.is_regular_file()) {
            GetNPcommID(trophyPath, index);

            Common::FS::IOFile file(it.path(), Common::FS::FileAccessMode::Read);
            if (!file.IsOpen()) {
                return false;
            }

            TrpHeader header;
            file.Read(header);
            if (header.magic != 0xDCA24D00)
                return false;

            s64 seekPos = sizeof(TrpHeader);
            std::filesystem::path trpFilesPath(
                Common::FS::GetUserPath(Common::FS::PathType::MetaDataDir) / title / "TrophyFiles" /
                it.path().stem());
            std::filesystem::create_directories(trpFilesPath / "Icons");
            std::filesystem::create_directory(trpFilesPath / "Xml");

            for (int i = 0; i < header.entry_num; i++) {
                if (!file.Seek(seekPos)) {
                    LOG_CRITICAL(Common_Filesystem, "Failed to seek to TRP entry offset");
                    return false;
                }
                seekPos += (s64)header.entry_size;
                TrpEntry entry;
                file.Read(entry);
                std::string_view name(entry.entry_name);
                if (entry.flag == 0 && name.find("TROP") != std::string::npos) { // PNG
                    if (file.Seek(entry.entry_pos)) {
                        LOG_CRITICAL(Common_Filesystem, "Failed to seek to TRP entry offset");
                        return false;
                    }
                    std::vector<u8> icon(entry.entry_len);
                    file.Read(icon);
                    Common::FS::IOFile::WriteBytes(trpFilesPath / "Icons" / name, icon);
                }
                if (entry.flag == 3 && np_comm_id[0] == 'N' &&
                    np_comm_id[1] == 'P') { // ESFM, encrypted.
                    if (file.Seek(entry.entry_pos)) {
                        LOG_CRITICAL(Common_Filesystem, "Failed to seek to TRP entry offset");
                        return false;
                    }
                    file.Read(esfmIv); // get iv key.
                    // Skip the first 16 bytes which are the iv key on every entry as we want a
                    // clean xml file.
                    std::vector<u8> ESFM(entry.entry_len - iv_len);
                    std::vector<u8> XML(entry.entry_len - iv_len);
                    if (file.Seek(entry.entry_pos + iv_len)) {
                        LOG_CRITICAL(Common_Filesystem, "Failed to seek to TRP entry + iv offset");
                        return false;
                    }
                    file.Read(ESFM);
                    crypto.decryptEFSM(np_comm_id, esfmIv, ESFM, XML); // decrypt
                    removePadding(XML);
                    std::string xml_name = entry.entry_name;
                    size_t pos = xml_name.find("ESFM");
                    if (pos != std::string::npos)
                        xml_name.replace(pos, xml_name.length(), "XML");
                    Common::FS::IOFile::WriteBytes(trpFilesPath / "Xml" / xml_name, XML);
                }
            }
        }
        index++;
    }
    return true;
}
