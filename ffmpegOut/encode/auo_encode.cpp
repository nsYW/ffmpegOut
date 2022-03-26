﻿// -----------------------------------------------------------------------------------------
// ffmpegOut by rigaya
// -----------------------------------------------------------------------------------------
// The MIT License
//
// Copyright (c) 2012-2017 rigaya
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// --------------------------------------------------------------------------------------------

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <Math.h>
#include <float.h>
#include <stdio.h>
#include <shlwapi.h>
#pragma comment(lib, "shlwapi.lib")
#include <vector>
#include <string>
#include <filesystem>
#include <memory>
#include <functional>

#include "auo.h"
#include "auo_version.h"
#include "auo_util.h"
#include "auo_conf.h"
#include "auo_settings.h"
#include "auo_system.h"
#include "auo_pipe.h"

#include "auo_frm.h"
#include "auo_encode.h"
#include "auo_error.h"
#include "auo_audio.h"
#include "auo_faw2aac.h"
#include "cpu_info.h"

using unique_handle = std::unique_ptr<std::remove_pointer<HANDLE>::type, std::function<void(HANDLE)>>;

static void create_aviutl_opened_file_list(PRM_ENC *pe);
static bool check_file_is_aviutl_opened_file(const char *filepath, const PRM_ENC *pe);

#pragma warning (push)
#pragma warning (disable: 4244)
#pragma warning (disable: 4996)
static inline std::string tolowercase(const std::string& str) {
    std::string str_copy = str;
    std::transform(str_copy.cbegin(), str_copy.cend(), str_copy.begin(), tolower);
    return str_copy;
}
#pragma warning (pop)

static std::vector<std::filesystem::path> find_exe_files(const char *target_dir) {
    std::vector<std::filesystem::path> ret;
    try {
        for (const std::filesystem::directory_entry& x : std::filesystem::recursive_directory_iterator(target_dir)) {
            if (x.path().extension() == ".exe") {
                ret.push_back(x.path());
            }
        }
    } catch (...) {}
    return ret;
}

static std::vector<std::filesystem::path> find_target_exe_files(const char *target_name, const std::vector<std::filesystem::path>& exe_files) {
    std::vector<std::filesystem::path> ret;
    const auto targetNameLower = tolowercase(std::filesystem::path(target_name).stem().string());
    for (const auto& path : exe_files) {
        if (tolowercase(path.stem().string()).substr(0, targetNameLower.length()) == targetNameLower) {
            ret.push_back(path);
        }
    }
    return ret;
}

static bool ends_with(const std::string& s, const std::string& check) {
    if (s.size() < check.size()) return false;
    return std::equal(std::rbegin(check), std::rend(check), std::rbegin(s));
}

static std::vector<std::filesystem::path> select_exe_file(const std::vector<std::filesystem::path>& pathList) {
    if (pathList.size() <= 1) {
        return pathList;
    }
    std::vector<std::filesystem::path> exe32bit;
    std::vector<std::filesystem::path> exe64bit;
    std::vector<std::filesystem::path> exeUnknown;
    for (const auto& path : pathList) {
        if (ends_with(tolowercase(path.filename().string()), "_x64.exe")) {
            exe64bit.push_back(path);
            continue;
        } else if (ends_with(tolowercase(path.filename().string()), "_x86.exe")) {
            exe32bit.push_back(path);
            continue;
        }
        bool checked = false;
        std::filesystem::path p = path;
        for (int i = 0; p.string().length() > 0 && i < 10000; i++) {
            auto parent = p.parent_path();
            if (parent == p) {
                break;
            }
            if (p.filename().string() == "x64") {
                exe64bit.push_back(path);
                checked = true;
                break;
            } else if (p.filename().string() == "x86") {
                exe32bit.push_back(path);
                checked = true;
                break;
            }
        }
        if (!checked) {
            if (ends_with(tolowercase(path.filename().string()), "64.exe")) {
                exe64bit.push_back(path);
            } else {
                exeUnknown.push_back(path);
            }
        }
    }
    if (is_64bit_os()) {
        return (exe64bit.size() > 0) ? exe64bit : exeUnknown;
    } else {
        return (exe32bit.size() > 0) ? exe32bit : exeUnknown;
    }
}

std::filesystem::path find_latest_ffmpeg(const std::vector<std::filesystem::path>& pathList) {
    if (pathList.size() == 0) {
        return std::filesystem::path();
    }
    auto selectedPathList = select_exe_file(pathList);
    if (selectedPathList.size() == 1) {
        return selectedPathList.front();
    }
#if 0
    int version = 0;
    std::filesystem::path ret;
    for (auto& path : selectedPathList) {
        int v = get_x264_rev(path.string().c_str());
        if (v >= version) {
            version = v;
            ret = path;
        }
    }
    return ret;
#else
    return selectedPathList.front();
#endif
}

void get_audio_pipe_name(char *pipename, size_t nSize, int audIdx) {
    sprintf_s(pipename, nSize, AUO_NAMED_PIPE_BASE, GetCurrentProcessId(), audIdx);
}

bool video_is_last_pass(const PRM_ENC *pe) {
    return pe->total_x264_pass == 0 || pe->current_x264_pass >= pe->total_x264_pass;
}

static BOOL check_muxer_exist(MUXER_SETTINGS *muxer_stg, const char *aviutl_dir, const BOOL get_relative_path, const std::vector<std::filesystem::path>& exe_files) {
    if (PathFileExists(muxer_stg->fullpath)) {
        info_use_exe_found(muxer_stg->dispname, muxer_stg->fullpath);
        return TRUE;
    }
    const auto targetExes = select_exe_file(find_target_exe_files(muxer_stg->filename, exe_files));
    if (targetExes.size() > 0) {
        if (get_relative_path) {
            GetRelativePathTo(muxer_stg->fullpath, _countof(muxer_stg->fullpath), targetExes.front().string().c_str(), FILE_ATTRIBUTE_NORMAL, aviutl_dir);
        } else {
            strcpy_s(muxer_stg->fullpath, targetExes.front().string().c_str());
        }
    }
    if (PathFileExists(muxer_stg->fullpath)) {
        info_use_exe_found(muxer_stg->dispname, muxer_stg->fullpath);
        return TRUE;
    }
    error_no_exe_file(muxer_stg->filename, muxer_stg->fullpath);
    return FALSE;
}

static BOOL muxer_supports_audio_format(const int muxer_to_be_used, const AUDIO_SETTINGS *aud_stg) {
    switch (muxer_to_be_used) {
    case MUXER_TC2MP4:
    case MUXER_MP4_RAW:
    case MUXER_MP4:
        return aud_stg->unsupported_mp4 == 0;
    case MUXER_MKV:
    case MUXER_MPG:
    case MUXER_DISABLED:
        return TRUE;
    default:
        return FALSE;
    }
}

BOOL check_if_exedit_is_used() {
    char name[256];
    wsprintf(name, "exedit_%d_%d", '01', GetCurrentProcessId());
    auto handle = unique_handle(OpenFileMapping(FILE_MAP_WRITE, FALSE, name),
        [](HANDLE h) { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); });

    return handle != nullptr;
}

static BOOL check_temp_file_open(const char *temp_filename, const char *defaultExeDir) {
    DWORD err = ERROR_SUCCESS;

    char exe_path[MAX_PATH_LEN] = { 0 };
    PathCombine(exe_path, defaultExeDir, AUO_CHECK_FILEOPEN_NAME);

    if (is_64bit_os() && !PathFileExists(exe_path)) {
        warning_no_auo_check_fileopen();
    }

    if (is_64bit_os() && PathFileExists(exe_path)) {
        //64bit OSでは、32bitアプリに対してはVirtualStoreが働く一方、
        //64bitアプリに対してはVirtualStoreが働かない
        //x264を64bitで実行することを考慮すると、
        //Aviutl(32bit)からチェックしても意味がないので、64bitプロセスからのチェックを行う
        PROCESS_INFORMATION pi;
        PIPE_SET pipes;
        InitPipes(&pipes);

        char fullargs[4096] = { 0 };
        sprintf_s(fullargs, "\"%s\" \"%s\"", exe_path, temp_filename);

        int ret = 0;
        if ((ret = RunProcess(fullargs, defaultExeDir, &pi, &pipes, NORMAL_PRIORITY_CLASS, TRUE, FALSE)) == RP_SUCCESS) {
            WaitForSingleObject(pi.hProcess, INFINITE);
            GetExitCodeProcess(pi.hProcess, &err);
            CloseHandle(pi.hProcess);
        }
        if (err == ERROR_SUCCESS) {
            return TRUE;
        }
    } else {
        auto handle = unique_handle(CreateFile(temp_filename, GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL),
            [](HANDLE h) { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); });
        if (handle.get() != INVALID_HANDLE_VALUE) {
            handle.reset();
            DeleteFile(temp_filename);
            return TRUE;
        }
        err = GetLastError();
    }
    if (err != ERROR_ALREADY_EXISTS) {
        char *mesBuffer = nullptr;
        FormatMessage(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            NULL, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            (LPSTR)&mesBuffer, 0, NULL);
        error_failed_to_open_tempfile(temp_filename, mesBuffer, err);
        if (mesBuffer != nullptr) {
            LocalFree(mesBuffer);
        }
    }
    return FALSE;
}

BOOL audio_encoder_exe_exists(const CONF_GUIEX *conf, const guiEx_settings *exstg) {
    const BOOL use_internal = conf->aud.use_internal;
    const int aud_idx = (use_internal) ? conf->aud.in.encoder : conf->aud.ext.encoder;
    const AUDIO_SETTINGS *aud_list = (use_internal) ? exstg->s_aud_int : exstg->s_aud_ext;
    const AUDIO_SETTINGS *aud_stg = &aud_list[aud_idx];
    if (!str_has_char(aud_stg->filename)) {
        return TRUE;
    }
    if (aud_idx == exstg->get_faw_index(use_internal)) {
        return TRUE;
    }
    return PathFileExists(aud_stg->fullpath);
}

BOOL check_output(CONF_GUIEX *conf, OUTPUT_INFO *oip, const PRM_ENC *pe, guiEx_settings *exstg) {
    BOOL check = TRUE;
    //ファイル名長さ
    if (strlen(oip->savefile) > (MAX_PATH_LEN - MAX_APPENDIX_LEN - 1)) {
        error_filename_too_long();
        check = FALSE;
    }

    char aviutl_dir[MAX_PATH_LEN] = { 0 };
    get_aviutl_dir(aviutl_dir, _countof(aviutl_dir));

    char defaultExeDir[MAX_PATH_LEN] = { 0 };
    PathCombineLong(defaultExeDir, _countof(defaultExeDir), aviutl_dir, DEFAULT_EXE_DIR);

    //ダメ文字・環境依存文字チェック
    char savedir[MAX_PATH_LEN] = { 0 };
    strcpy_s(savedir, oip->savefile);
    PathRemoveFileSpecFixed(savedir);
    if (!PathIsDirectory(savedir)) {
        error_savdir_do_not_exist(oip->savefile, savedir);
        check = FALSE;
        //一時ファイルを開けるかどうか
    } else if (!check_temp_file_open(pe->temp_filename, defaultExeDir)) {
        check = FALSE;
    }

    if (check_file_is_aviutl_opened_file(oip->savefile, pe)) {
        error_file_is_already_opened_by_aviutl();
        check = FALSE;
    }

    //解像度
    int w_mul = 1, h_mul = 1;
    switch (conf->enc.output_csp) {
        case OUT_CSP_YUV444:
        case OUT_CSP_RGB:
        case OUT_CSP_RGBA:
            w_mul = 1, h_mul = 1; break;
        case OUT_CSP_NV16:
            w_mul = 2, h_mul = 1; break;
        case OUT_CSP_NV12:
        default:
            w_mul = 2; h_mul = 2; break;
    }
    if (conf->enc.interlaced) h_mul *= 2;
    if (oip->w % w_mul) {
        error_invalid_resolution(TRUE,  w_mul, oip->w, oip->h);
        check = FALSE;
    }
    if (oip->h % h_mul) {
        error_invalid_resolution(FALSE, h_mul, oip->w, oip->h);
        check = FALSE;
    }

    //出力するもの
    if (pe->video_out_type == VIDEO_OUTPUT_DISABLED && !(oip->flag & OUTPUT_INFO_FLAG_AUDIO)) {
        error_nothing_to_output();
        check = FALSE;
    }
    if (pe->video_out_type != VIDEO_OUTPUT_DISABLED && oip->n <= 0) {
        error_output_zero_frames();
        check = FALSE;
    }

    if (conf->oth.out_audio_only)
        write_log_auo_line(LOG_INFO, "音声のみ出力を行います。");

    const auto exeFiles = find_exe_files(defaultExeDir);

    //必要な実行ファイル
    //ffmpegout
    if (!conf->oth.disable_guicmd && pe->video_out_type != VIDEO_OUTPUT_DISABLED) {
        if (!PathFileExists(exstg->s_local.ffmpeg_path)) {
            const auto targetExes = find_target_exe_files(ENCODER_NAME, exeFiles);
            if (targetExes.size() > 0) {
                const auto latestVidEnc = find_latest_ffmpeg(targetExes);
                if (exstg->s_local.get_relative_path) {
                    GetRelativePathTo(exstg->s_local.ffmpeg_path, _countof(exstg->s_local.ffmpeg_path), latestVidEnc.string().c_str(), FILE_ATTRIBUTE_NORMAL, aviutl_dir);
                } else {
                    strcpy_s(exstg->s_local.ffmpeg_path, latestVidEnc.string().c_str());
                }
            }
            if (!PathFileExists(exstg->s_local.ffmpeg_path)) {
                error_no_exe_file(ENCODER_NAME, exstg->s_local.ffmpeg_path);
                check = FALSE;
            }
        }
        info_use_exe_found(ENCODER_NAME, exstg->s_local.ffmpeg_path);
    }

    //音声エンコーダ
    if (oip->flag & OUTPUT_INFO_FLAG_AUDIO) {
        //音声長さチェック
        check_audio_length(oip);

        if (conf->aud.use_internal) {
            CONF_AUDIO_BASE *cnf_aud = &conf->aud.in;
            cnf_aud->audio_encode_timing = 2;
            cnf_aud->delay_cut = AUDIO_DELAY_CUT_NONE;

            const bool default_audenc_cnf_avail = (exstg->s_local.default_audio_encoder_in < exstg->s_aud_int_count
                && str_has_char(exstg->s_aud_int[exstg->s_local.default_audio_encoder_in].filename));
            const bool default_audenc_auo_avail = (DEFAULT_AUDIO_ENCODER_IN < exstg->s_aud_int_count
                && str_has_char(exstg->s_aud_int[DEFAULT_AUDIO_ENCODER_IN].filename));
            if (cnf_aud->encoder < 0 || exstg->s_aud_int_count <= cnf_aud->encoder) {
                if (default_audenc_cnf_avail) {
                    cnf_aud->encoder = exstg->s_local.default_audio_encoder_ext;
                    warning_use_default_audio_encoder(exstg->s_aud_int[cnf_aud->encoder].dispname);
                } else if (default_audenc_auo_avail) {
                    cnf_aud->encoder = DEFAULT_AUDIO_ENCODER_IN;
                    warning_use_default_audio_encoder(exstg->s_aud_int[cnf_aud->encoder].dispname);
                }
            }
            if (cnf_aud->encoder < 0 || exstg->s_aud_int_count <= cnf_aud->encoder) {
                error_invalid_ini_file();
                check = FALSE;
            }
#if 0
            AUDIO_SETTINGS *aud_stg = &exstg->s_aud_int[cnf_aud->encoder];
            if (!muxer_supports_audio_format(pe->muxer_to_be_used, aud_stg)) {
                AUDIO_SETTINGS *aud_default = nullptr;
                if (default_audenc_cnf_avail) {
                    aud_default = &exstg->s_aud_ext[exstg->s_local.default_audio_encoder_ext];
                } else if (default_audenc_auo_avail) {
                    aud_default = &exstg->s_aud_ext[DEFAULT_AUDIO_ENCODER_EXT];
                }
                error_unsupported_audio_format_by_muxer(pe->video_out_type, aud_stg->dispname, (aud_default) ? aud_default->dispname : nullptr);
                check = FALSE;
            }
#endif
        } else {
            CONF_AUDIO_BASE *cnf_aud = &conf->aud.ext;
            const bool default_audenc_cnf_avail = (exstg->s_local.default_audio_encoder_ext < exstg->s_aud_ext_count
                && str_has_char(exstg->s_aud_ext[exstg->s_local.default_audio_encoder_ext].filename));
            const bool default_audenc_auo_avail = (DEFAULT_AUDIO_ENCODER_EXT < exstg->s_aud_ext_count
                && str_has_char(exstg->s_aud_ext[DEFAULT_AUDIO_ENCODER_EXT].filename));
            if ((cnf_aud->encoder < 0 || exstg->s_aud_ext_count <= cnf_aud->encoder)) {
                if (default_audenc_cnf_avail) {
                    cnf_aud->encoder = exstg->s_local.default_audio_encoder_ext;
                    warning_use_default_audio_encoder(exstg->s_aud_ext[cnf_aud->encoder].dispname);
                } else if (default_audenc_auo_avail) {
                    cnf_aud->encoder = DEFAULT_AUDIO_ENCODER_EXT;
                    warning_use_default_audio_encoder(exstg->s_aud_ext[cnf_aud->encoder].dispname);
                }
            }
            if (0 <= cnf_aud->encoder && cnf_aud->encoder < exstg->s_aud_ext_count) {
                AUDIO_SETTINGS *aud_stg = &exstg->s_aud_ext[cnf_aud->encoder];
                if (!audio_encoder_exe_exists(conf, exstg)) {
                    //とりあえず、exe_filesを探す
                    {
                        const auto targetExes = select_exe_file(find_target_exe_files(aud_stg->filename, exeFiles));
                        if (targetExes.size() > 0) {
                            if (exstg->s_local.get_relative_path) {
                                GetRelativePathTo(aud_stg->fullpath, _countof(aud_stg->fullpath), targetExes.front().string().c_str(), FILE_ATTRIBUTE_NORMAL, aviutl_dir);
                            } else {
                                strcpy_s(aud_stg->fullpath, targetExes.front().string().c_str());
                            }
                        }
                    }
                    //みつからなければ、デフォルトエンコーダを探す
                    if (!PathFileExists(aud_stg->fullpath) && default_audenc_cnf_avail) {
                        cnf_aud->encoder = exstg->s_local.default_audio_encoder_ext;
                        aud_stg = &exstg->s_aud_ext[cnf_aud->encoder];
                        if (!PathFileExists(aud_stg->fullpath)) {
                            const auto targetExes = select_exe_file(find_target_exe_files(aud_stg->filename, exeFiles));
                            if (targetExes.size() > 0) {
                                if (exstg->s_local.get_relative_path) {
                                    GetRelativePathTo(aud_stg->fullpath, _countof(aud_stg->fullpath), targetExes.front().string().c_str(), FILE_ATTRIBUTE_NORMAL, aviutl_dir);
                                } else {
                                    strcpy_s(aud_stg->fullpath, targetExes.front().string().c_str());
                                }
                                warning_use_default_audio_encoder(aud_stg->dispname);
                            }
                        }
                    }
                    if (!PathFileExists(aud_stg->fullpath) && default_audenc_auo_avail) {
                        cnf_aud->encoder = DEFAULT_AUDIO_ENCODER_EXT;
                        aud_stg = &exstg->s_aud_ext[cnf_aud->encoder];
                        if (!PathFileExists(aud_stg->fullpath)) {
                            const auto targetExes = select_exe_file(find_target_exe_files(aud_stg->filename, exeFiles));
                            if (targetExes.size() > 0) {
                                if (exstg->s_local.get_relative_path) {
                                    GetRelativePathTo(aud_stg->fullpath, _countof(aud_stg->fullpath), targetExes.front().string().c_str(), FILE_ATTRIBUTE_NORMAL, aviutl_dir);
                                } else {
                                    strcpy_s(aud_stg->fullpath, targetExes.front().string().c_str());
                                }
                                warning_use_default_audio_encoder(aud_stg->dispname);
                            }
                        }
                    }
                    if (!PathFileExists(aud_stg->fullpath)) {
                        //fawの場合はfaw2aacがあればOKだが、それもなければエラー
                        if (!(cnf_aud->encoder == exstg->get_faw_index(conf->aud.use_internal) && check_if_faw2aac_exists())) {
                            error_no_exe_file(aud_stg->filename, aud_stg->fullpath);
                            check = FALSE;
                        }
                    }
                }
                if (str_has_char(aud_stg->filename) && (cnf_aud->encoder != exstg->get_faw_index(conf->aud.use_internal) || !check_if_faw2aac_exists())) {
                    info_use_exe_found("音声エンコーダ", aud_stg->fullpath);
                }
#if 0
                if (!muxer_supports_audio_format(pe->muxer_to_be_used, aud_stg)) {
                    AUDIO_SETTINGS *aud_default = nullptr;
                    if (default_audenc_cnf_avail) {
                        aud_default = &exstg->s_aud_ext[exstg->s_local.default_audio_encoder_ext];
                    } else if (default_audenc_auo_avail) {
                        aud_default = &exstg->s_aud_ext[DEFAULT_AUDIO_ENCODER_EXT];
                    }
                    error_unsupported_audio_format_by_muxer(pe->video_out_type, aud_stg->dispname, (aud_default) ? aud_default->dispname : nullptr);
                    check = FALSE;
                }
#endif
            } else {
                error_invalid_ini_file();
                check = FALSE;
            }
        }
    }

    //muxer
    switch (pe->muxer_to_be_used) {
        case MUXER_TC2MP4:
            check &= check_muxer_exist(&exstg->s_mux[MUXER_MP4], aviutl_dir, exstg->s_local.get_relative_path, exeFiles); //tc2mp4使用時は追加でmp4boxも必要
            //下へフォールスルー
        case MUXER_MP4:
            check &= check_muxer_exist(&exstg->s_mux[MUXER_MP4], aviutl_dir, exstg->s_local.get_relative_path, exeFiles);
            if (str_has_char(exstg->s_mux[MUXER_MP4_RAW].base_cmd)) {
                check &= check_muxer_exist(&exstg->s_mux[MUXER_MP4_RAW], aviutl_dir, exstg->s_local.get_relative_path, exeFiles);
            }
            //check &= check_muxer_matched_with_ini(exstg->s_mux);
            break;
        case MUXER_MKV:
            check &= check_muxer_exist(&exstg->s_mux[pe->muxer_to_be_used], aviutl_dir, exstg->s_local.get_relative_path, exeFiles);
            break;
        default:
            break;
    }

    return check;
}

void open_log_window(const char *savefile, const SYSTEM_DATA *sys_dat, int current_pass, int total_pass) {
    char mes[MAX_PATH_LEN + 512];
    char *newLine = (get_current_log_len(current_pass)) ? "\r\n\r\n" : ""; //必要なら行送り
    static const char *SEPARATOR = "------------------------------------------------------------------------------------------------------------------------------";
    if (total_pass < 2)
        sprintf_s(mes, sizeof(mes), "%s%s\r\n[%s]\r\n%s", newLine, SEPARATOR, savefile, SEPARATOR);
    else
        sprintf_s(mes, sizeof(mes), "%s%s\r\n[%s] (%d / %d pass)\r\n%s", newLine, SEPARATOR, savefile, current_pass, total_pass, SEPARATOR);
    
    show_log_window(sys_dat->aviutl_dir, sys_dat->exstg->s_local.disable_visual_styles);
    write_log_line(LOG_INFO, mes);
    
    char cpu_info[256];
    getCPUInfo(cpu_info);
    DWORD buildNumber = 0;
    const TCHAR *osver = getOSVersion(&buildNumber);
    write_log_auo_line_fmt(LOG_INFO, "%s %s / %s %s (%d) / %s", AUO_NAME_WITHOUT_EXT, AUO_VERSION_STR, osver, is_64bit_os() ? "x64" : "x86", buildNumber, cpu_info);
}

static void set_tmpdir(PRM_ENC *pe, int tmp_dir_index, const char *savefile, const SYSTEM_DATA *sys_dat) {
    if (tmp_dir_index < TMP_DIR_OUTPUT || TMP_DIR_CUSTOM < tmp_dir_index)
        tmp_dir_index = TMP_DIR_OUTPUT;

    if (tmp_dir_index == TMP_DIR_SYSTEM) {
        //システムの一時フォルダを取得
        if (GetTempPath(_countof(pe->temp_filename), pe->temp_filename) != NULL) {
            PathRemoveBackslash(pe->temp_filename);
            write_log_auo_line_fmt(LOG_INFO, "一時フォルダ : %s", pe->temp_filename);
        } else {
            warning_failed_getting_temp_path();
            tmp_dir_index = TMP_DIR_OUTPUT;
        }
    }
    if (tmp_dir_index == TMP_DIR_CUSTOM) {
        //指定されたフォルダ
        if (DirectoryExistsOrCreate(sys_dat->exstg->s_local.custom_tmp_dir)) {
            strcpy_s(pe->temp_filename, GetFullPathFrom(sys_dat->exstg->s_local.custom_tmp_dir, sys_dat->aviutl_dir).c_str());
            PathRemoveBackslash(pe->temp_filename);
            write_log_auo_line_fmt(LOG_INFO, "一時フォルダ : %s", pe->temp_filename);
        } else {
            warning_no_temp_root(sys_dat->exstg->s_local.custom_tmp_dir);
            tmp_dir_index = TMP_DIR_OUTPUT;
        }
    }
    if (tmp_dir_index == TMP_DIR_OUTPUT) {
        //出力フォルダと同じ("\"なし)
        strcpy_s(pe->temp_filename, _countof(pe->temp_filename), savefile);
        PathRemoveFileSpecFixed(pe->temp_filename);
    }
}

static void set_aud_delay_cut(CONF_GUIEX *conf, PRM_ENC *pe, const OUTPUT_INFO *oip, const SYSTEM_DATA *sys_dat) {
    pe->delay_cut_additional_vframe = 0;
    pe->delay_cut_additional_aframe = 0;
    if (oip->flag & OUTPUT_INFO_FLAG_AUDIO) {
        if (conf->aud.use_internal) {
            conf->aud.in.delay_cut = AUDIO_DELAY_CUT_NONE;
        } else {
            CONF_AUDIO_BASE *cnf_aud = &conf->aud.ext;
            const AUDIO_SETTINGS *aud_stg = &sys_dat->exstg->s_aud_ext[cnf_aud->encoder];
            int audio_delay = aud_stg->mode[cnf_aud->enc_mode].delay;
            if (audio_delay) {
                const double fps = oip->rate / (double)oip->scale;
                const int audio_rate = oip->audio_rate;
                switch (cnf_aud->delay_cut) {
                case AUDIO_DELAY_CUT_DELETE_AUDIO:
                    pe->delay_cut_additional_aframe = -1 * audio_delay;
                    break;
                case AUDIO_DELAY_CUT_ADD_VIDEO:
                    pe->delay_cut_additional_vframe = additional_vframe_for_aud_delay_cut(fps, audio_rate, audio_delay);
                    pe->delay_cut_additional_aframe = additional_silence_for_aud_delay_cut(fps, audio_rate, audio_delay);
                    break;
                case AUDIO_DELAY_CUT_NONE:
                default:
                    break;
                }
            } else {
                cnf_aud->delay_cut = AUDIO_DELAY_CUT_NONE;
            }
        }
    }
}

int get_total_path(const CONF_GUIEX *conf) {
    return (conf->enc.use_auto_npass
         //&& conf->enc.rc_mode == X264_RC_BITRATE
         && !conf->oth.disable_guicmd)
         ? conf->enc.auto_npass : 1;
}

void avoid_exsisting_tmp_file(char *buf, size_t size) {
    if (!PathFileExists(buf)) {
        return;
    }
    char tmp[MAX_PATH_LEN];
    for (int i = 0; i < 1000000; i++) {
        char new_ext[32];
        sprintf_s(new_ext, ".%d%s", i, PathFindExtension(buf));
        strcpy_s(tmp, buf);
        change_ext(tmp, size, new_ext);
        if (!PathFileExists(tmp)) {
            strcpy_s(buf, size, tmp);
            return;
        }
    }
}

void free_enc_prm(PRM_ENC *pe) {
    if (pe->opened_aviutl_files) {
        for (int i = 0; i < pe->n_opened_aviutl_files; i++) {
            if (pe->opened_aviutl_files[i]) {
                free(pe->opened_aviutl_files[i]);
            }
        }
        free(pe->opened_aviutl_files);
        pe->opened_aviutl_files = nullptr;
        pe->n_opened_aviutl_files = 0;
    }
}

void set_enc_prm(CONF_GUIEX *conf, PRM_ENC *pe, const OUTPUT_INFO *oip, const SYSTEM_DATA *sys_dat) {
    //初期化
    ZeroMemory(pe, sizeof(PRM_ENC));
    //設定更新
    sys_dat->exstg->load_encode_stg();
    sys_dat->exstg->load_append();
    sys_dat->exstg->load_fn_replace();

    pe->video_out_type = check_video_ouput(conf, oip);
    pe->muxer_to_be_used = check_muxer_to_be_used(conf, pe->video_out_type, (oip->flag & OUTPUT_INFO_FLAG_AUDIO) != 0);
    pe->total_x264_pass = get_total_path(conf);
    //pe->amp_x264_pass_limit = pe->total_x264_pass + sys_dat->exstg->s_local.amp_retry_limit;
    pe->current_x264_pass = 1;
    pe->drop_count = 0;
    memcpy(&pe->append, &sys_dat->exstg->s_append, sizeof(FILE_APPENDIX));
    ZeroMemory(&pe->append.aud, sizeof(pe->append.aud));
    create_aviutl_opened_file_list(pe);

    char filename_replace[MAX_PATH_LEN];

    //一時フォルダの決定
    set_tmpdir(pe, conf->oth.temp_dir, oip->savefile, sys_dat);

    //音声一時フォルダの決定
    char *cus_aud_tdir = pe->temp_filename;
    if (!conf->aud.use_internal) {
        if (conf->aud.ext.aud_temp_dir) {
            if (DirectoryExistsOrCreate(sys_dat->exstg->s_local.custom_audio_tmp_dir)) {
                cus_aud_tdir = sys_dat->exstg->s_local.custom_audio_tmp_dir;
                write_log_auo_line_fmt(LOG_INFO, "音声一時フォルダ : %s", GetFullPathFrom(cus_aud_tdir, sys_dat->aviutl_dir).c_str());
            } else {
                warning_no_aud_temp_root(sys_dat->exstg->s_local.custom_audio_tmp_dir);
            }
        }
        strcpy_s(pe->aud_temp_dir, GetFullPathFrom(cus_aud_tdir, sys_dat->aviutl_dir).c_str());
    }

    //ファイル名置換を行い、一時ファイル名を作成
    strcpy_s(filename_replace, _countof(filename_replace), PathFindFileName(oip->savefile));
    sys_dat->exstg->apply_fn_replace(filename_replace, _countof(filename_replace));
    PathCombineLong(pe->temp_filename, _countof(pe->temp_filename), pe->temp_filename, filename_replace);
    //ファイルの上書きを避ける
    avoid_exsisting_tmp_file(pe->temp_filename, _countof(pe->temp_filename));

    //FAWチェックとオーディオディレイの修正
    const CONF_AUDIO_BASE *cnf_aud = (conf->aud.use_internal) ? &conf->aud.in : &conf->aud.ext;
    if (cnf_aud->faw_check)
        auo_faw_check(&conf->aud, oip, pe, sys_dat->exstg);
    set_aud_delay_cut(conf, pe, oip, sys_dat);
}

void auto_save_log(const CONF_GUIEX *conf, const OUTPUT_INFO *oip, const PRM_ENC *pe, const SYSTEM_DATA *sys_dat) {
    guiEx_settings ex_stg(true);
    ex_stg.load_log_win();
    if (!ex_stg.s_log.auto_save_log)
        return;
    char log_file_path[MAX_PATH_LEN];
    if (AUO_RESULT_SUCCESS != getLogFilePath(log_file_path, _countof(log_file_path), pe, sys_dat, conf, oip))
        warning_no_auto_save_log_dir();
    auto_save_log_file(log_file_path);
    return;
}

int additional_vframe_for_aud_delay_cut(double fps, int audio_rate, int audio_delay) {
    double delay_sec = audio_delay / (double)audio_rate;
    return (int)ceil(delay_sec * fps);
}

int additional_silence_for_aud_delay_cut(double fps, int audio_rate, int audio_delay, int vframe_added) {
    vframe_added = (vframe_added >= 0) ? vframe_added : additional_vframe_for_aud_delay_cut(fps, audio_rate, audio_delay);
    return (int)(vframe_added / (double)fps * audio_rate + 0.5) - audio_delay;
}

BOOL fps_after_afs_is_24fps(const int frame_n, const PRM_ENC *pe) {
    return (pe->drop_count > (frame_n * 0.10));
}

int get_mux_excmd_mode(const CONF_GUIEX *conf, const PRM_ENC *pe) {
    int mode = 0;
    switch (pe->muxer_to_be_used) {
        case MUXER_MKV:     mode = conf->mux.mkv_mode; break;
        case MUXER_MPG:     mode = conf->mux.mpg_mode; break;
        case MUXER_MP4:
        case MUXER_TC2MP4: 
        case MUXER_MP4_RAW: mode = conf->mux.mp4_mode; break;
    }
    return mode;
}

void get_aud_filename(char *audfile, size_t nSize, const PRM_ENC *pe, int i_aud) {
    PathCombineLong(audfile, nSize, pe->aud_temp_dir, PathFindFileName(pe->temp_filename));
    apply_appendix(audfile, nSize, audfile, pe->append.aud[i_aud]);
}

static void get_muxout_appendix(char *muxout_appendix, size_t nSize, const SYSTEM_DATA *sys_dat, const PRM_ENC *pe) {
    static const char * const MUXOUT_APPENDIX = "_muxout";
    strcpy_s(muxout_appendix, nSize, MUXOUT_APPENDIX);
    const char *ext = (pe->muxer_to_be_used >= 0 && str_has_char(sys_dat->exstg->s_mux[pe->muxer_to_be_used].out_ext)) ?
        sys_dat->exstg->s_mux[pe->muxer_to_be_used].out_ext : PathFindExtension(pe->temp_filename);
    strcat_s(muxout_appendix, nSize, ext);
}

void get_muxout_filename(char *filename, size_t nSize, const SYSTEM_DATA *sys_dat, const PRM_ENC *pe) {
    char muxout_appendix[MAX_APPENDIX_LEN];
    get_muxout_appendix(muxout_appendix, sizeof(muxout_appendix), sys_dat, pe);
    apply_appendix(filename, nSize, pe->temp_filename, muxout_appendix);
}

//チャプターファイル名とapple形式のチャプターファイル名を同時に作成する
void set_chap_filename(char *chap_file, size_t cf_nSize, char *chap_apple, size_t ca_nSize, const char *chap_base,
                       const PRM_ENC *pe, const SYSTEM_DATA *sys_dat, const CONF_GUIEX *conf, const OUTPUT_INFO *oip) {
    strcpy_s(chap_file, cf_nSize, chap_base);
    cmd_replace(chap_file, cf_nSize, pe, sys_dat, conf, oip);
    apply_appendix(chap_apple, ca_nSize, chap_file, pe->append.chap_apple);
    sys_dat->exstg->apply_fn_replace(PathFindFileName(chap_apple), ca_nSize - (PathFindFileName(chap_apple) - chap_apple));
}

void insert_num_to_replace_key(char *key, size_t nSize, int num) {
    char tmp[128];
    int key_len = strlen(key);
    sprintf_s(tmp, _countof(tmp), "%d%s", num, &key[key_len-1]);
    key[key_len-1] = '\0';
    strcat_s(key, nSize, tmp);
}

//static void replace_aspect_ratio(char *cmd, size_t nSize, const CONF_GUIEX *conf, const OUTPUT_INFO *oip) {
//	const int w = oip->w;
//	const int h = oip->h;
// 
//	int sar_x = conf->x264.sar.x;
//	int sar_y = conf->x264.sar.y;
//	int dar_x = 0;
//	int dar_y = 0;
//	if (sar_x * sar_y > 0) {
//		if (sar_x < 0) {
//			dar_x = -1 * sar_x;
//			dar_y = -1 * sar_y;
//			set_guiEx_auto_sar(&sar_x, &sar_y, w, h);
//		} else {
//			dar_x = sar_x * w;
//			dar_y = sar_y * h;
//			const int gcd = get_gcd(dar_x, dar_y);
//			dar_x /= gcd;
//			dar_y /= gcd;
//		}
//	}
//	if (sar_x * sar_y <= 0)
//		sar_x = sar_y = 1;
//	if (dar_x * dar_y <= 0)
//		dar_x = dar_y = 1;
//
//	char buf[32];
//	//%{sar_x} / %{par_x}
//	sprintf_s(buf, _countof(buf), "%d", sar_x);
//	replace(cmd, nSize, "%{sar_x}", buf);
//	replace(cmd, nSize, "%{par_x}", buf);
//	//%{sar_x} / %{sar_y}
//	sprintf_s(buf, _countof(buf), "%d", sar_y);
//	replace(cmd, nSize, "%{sar_y}", buf);
//	replace(cmd, nSize, "%{par_y}", buf);
//	//%{dar_x}
//	sprintf_s(buf, _countof(buf), "%d", dar_x);
//	replace(cmd, nSize, "%{dar_x}", buf);
//	//%{dar_y}
//	sprintf_s(buf, _countof(buf), "%d", dar_y);
//	replace(cmd, nSize, "%{dar_y}", buf);
//}

void cmd_replace(char *cmd, size_t nSize, const PRM_ENC *pe, const SYSTEM_DATA *sys_dat, const CONF_GUIEX *conf, const OUTPUT_INFO *oip) {
    char tmp[MAX_PATH_LEN] = { 0 };
    //置換操作の実行
    //%{vidpath}
    replace(cmd, nSize, "%{vidpath}", pe->temp_filename);
    //%{audpath}
    for (int i_aud = 0; i_aud < pe->aud_count; i_aud++) {
        if (str_has_char(pe->append.aud[i_aud])) {
            get_aud_filename(tmp, _countof(tmp), pe, i_aud);
            char aud_key[128] = "%{audpath}";
            if (i_aud)
                insert_num_to_replace_key(aud_key, _countof(aud_key), i_aud);
            replace(cmd, nSize, aud_key, tmp);
        }
    }
    //%{tmpdir}
    strcpy_s(tmp, _countof(tmp), pe->temp_filename);
    PathRemoveFileSpecFixed(tmp);
    PathForceRemoveBackSlash(tmp);
    replace(cmd, nSize, "%{tmpdir}", tmp);
    //%{tmpfile}
    strcpy_s(tmp, _countof(tmp), pe->temp_filename);
    PathRemoveExtension(tmp);
    replace(cmd, nSize, "%{tmpfile}", tmp);
    //%{tmpname}
    strcpy_s(tmp, _countof(tmp), PathFindFileName(pe->temp_filename));
    PathRemoveExtension(tmp);
    replace(cmd, nSize, "%{tmpname}", tmp);
    //%{savpath}
    replace(cmd, nSize, "%{savpath}", oip->savefile);
    //%{savfile}
    strcpy_s(tmp, _countof(tmp), oip->savefile);
    PathRemoveExtension(tmp);
    replace(cmd, nSize, "%{savfile}", tmp);
    //%{savname}
    strcpy_s(tmp, _countof(tmp), PathFindFileName(oip->savefile));
    PathRemoveExtension(tmp);
    replace(cmd, nSize, "%{savname}", tmp);
    //%{savdir}
    strcpy_s(tmp, _countof(tmp), oip->savefile);
    PathRemoveFileSpecFixed(tmp);
    PathForceRemoveBackSlash(tmp);
    replace(cmd, nSize, "%{savdir}", tmp);
    //%{aviutldir}
    strcpy_s(tmp, _countof(tmp), sys_dat->aviutl_dir);
    PathForceRemoveBackSlash(tmp);
    replace(cmd, nSize, "%{aviutldir}", tmp);
    //%{chpath}
    apply_appendix(tmp, _countof(tmp), oip->savefile, pe->append.chap);
    replace(cmd, nSize, "%{chpath}", tmp);
    //%{tcpath}
    apply_appendix(tmp, _countof(tmp), pe->temp_filename, pe->append.tc);
    replace(cmd, nSize, "%{tcpath}", tmp);
    //%{muxout}
    get_muxout_filename(tmp, _countof(tmp), sys_dat, pe);
    replace(cmd, nSize, "%{muxout}", tmp);
    //%{fps_rate}
    int fps_rate = oip->rate;
    int fps_scale = oip->scale;
    const int fps_gcd = get_gcd(fps_rate, fps_scale);
    fps_rate /= fps_gcd;
    fps_scale /= fps_gcd;
    sprintf_s(tmp, sizeof(tmp), "%d", fps_rate);
    replace(cmd, nSize, "%{fps_rate}", tmp);
    //%{fps_rate_times_4}
    fps_rate *= 4;
    sprintf_s(tmp, sizeof(tmp), "%d", fps_rate);
    replace(cmd, nSize, "%{fps_rate_times_4}", tmp);
    //%{fps_scale}
    sprintf_s(tmp, sizeof(tmp), "%d", fps_scale);
    replace(cmd, nSize, "%{fps_scale}", tmp);
    //アスペクト比
    //replace_aspect_ratio(cmd, nSize, conf, oip);

    if (conf->aud.use_internal) {
        replace(cmd, nSize, "%{audencpath}", "");
    } else {
        const CONF_AUDIO_BASE *cnf_aud = &conf->aud.ext;
        const AUDIO_SETTINGS *aud_stg = &sys_dat->exstg->s_aud_ext[cnf_aud->encoder];
        replace(cmd, nSize, "%{audencpath}", GetFullPathFrom(aud_stg->fullpath, sys_dat->aviutl_dir).c_str());
    }
    replace(cmd, nSize, "%{mp4muxerpath}", GetFullPathFrom(sys_dat->exstg->s_mux[MUXER_MP4].fullpath, sys_dat->aviutl_dir).c_str());
    replace(cmd, nSize, "%{mkvmuxerpath}", GetFullPathFrom(sys_dat->exstg->s_mux[MUXER_MKV].fullpath, sys_dat->aviutl_dir).c_str());
}

//一時ファイルの移動・削除を行う
// move_from -> move_to
// temp_filename … 動画ファイルの一時ファイル名。これにappendixをつけてmove_from を作る。
//                  appndixがNULLのときはこれをそのままmove_fromとみなす。
// appendix      … ファイルの後修飾子。NULLも可。
// savefile      … 保存動画ファイル名。これにappendixをつけてmove_to を作る。NULLだと move_to に移動できない。
// ret, erase    … これまでのエラーと一時ファイルを削除するかどうか。エラーがない場合にのみ削除できる
// name          … 一時ファイルの種類の名前
// must_exist    … trueのとき、移動するべきファイルが存在しないとエラーを返し、ファイルが存在しないことを伝える
static BOOL move_temp_file(const char *appendix, const char *temp_filename, const char *savefile, DWORD ret, BOOL erase, const char *name, BOOL must_exist) {
    char move_from_tmp[MAX_PATH_LEN] = { 0 };
    if (appendix)
        apply_appendix(move_from_tmp, _countof(move_from_tmp), temp_filename, appendix);
    else
        strcpy_s(move_from_tmp, temp_filename);

    char move_from[MAX_PATH_LEN] = { 0 };
    if (strcmp(name, "出力") == 0) {
        // 連番出力等の場合、1番が出ているかだけチェックする
        sprintf_s(move_from, move_from_tmp, 1);
        // ファイル名が変わっている(=連番出力等の場合)この後の処理をスキップ
        if (strcmp(move_from, move_from_tmp) != 0) {
            return TRUE;
        }
    } else {
        strcpy_s(move_from, move_from_tmp);
    }

    if (!PathFileExists(move_from)) {
        if (must_exist)
            write_log_auo_line_fmt(LOG_WARNING, "%sファイルが見つかりませんでした。", name);
        return (must_exist) ? FALSE : TRUE;
    }
    if (ret == AUO_RESULT_SUCCESS && erase) {
        remove(move_from);
        return TRUE;
    }
    if (savefile == NULL || appendix == NULL)
        return TRUE;
    char move_to[MAX_PATH_LEN] = { 0 };
    apply_appendix(move_to, _countof(move_to), savefile, appendix);
    if (_stricmp(move_from, move_to) != NULL) {
        if (PathFileExists(move_to))
            remove(move_to);
        if (rename(move_from, move_to))
            write_log_auo_line_fmt(LOG_WARNING, "%sファイルの移動に失敗しました。", name);
    }
    return TRUE;
}

AUO_RESULT move_temporary_files(const CONF_GUIEX *conf, const PRM_ENC *pe, const SYSTEM_DATA *sys_dat, const OUTPUT_INFO *oip, DWORD ret) {
    //動画ファイル
    if (!conf->oth.out_audio_only)
        if (!move_temp_file(PathFindExtension((pe->muxer_to_be_used >= 0) ? oip->savefile : pe->temp_filename), pe->temp_filename, oip->savefile, ret, FALSE, "出力", !ret))
            ret |= AUO_RESULT_ERROR;
    //動画のみファイル
    //if (str_has_char(pe->muxed_vid_filename) && PathFileExists(pe->muxed_vid_filename))
    //	remove(pe->muxed_vid_filename);
    //mux後ファイル
    if (pe->muxer_to_be_used >= 0) {
        char muxout_appendix[MAX_APPENDIX_LEN];
        get_muxout_appendix(muxout_appendix, _countof(muxout_appendix), sys_dat, pe);
        move_temp_file(muxout_appendix, pe->temp_filename, oip->savefile, ret, FALSE, "mux後ファイル", FALSE);
    }
    /*
    //qpファイル
    move_temp_file(pe->append.qp,   pe->temp_filename, oip->savefile, ret, !sys_dat->exstg->s_local.keep_qp_file, "qp", FALSE);
    //tcファイル
    BOOL erase_tc = conf->vid.afs && !conf->vid.auo_tcfile_out && pe->muxer_to_be_used != MUXER_DISABLED;
    move_temp_file(pe->append.tc,   pe->temp_filename, oip->savefile, ret, erase_tc, "タイムコード", FALSE);
    //チャプターファイル
    if (pe->muxer_to_be_used >= 0 && sys_dat->exstg->s_local.auto_del_chap) {
        char chap_file[MAX_PATH_LEN];
        char chap_apple[MAX_PATH_LEN];
        const MUXER_CMD_EX *muxer_mode = &sys_dat->exstg->s_mux[pe->muxer_to_be_used].ex_cmd[(pe->muxer_to_be_used == MUXER_MKV) ? conf->mux.mkv_mode : conf->mux.mp4_mode];
        set_chap_filename(chap_file, _countof(chap_file), chap_apple, _countof(chap_apple), muxer_mode->chap_file, pe, sys_dat, conf, oip);
        move_temp_file(NULL, chap_file,  NULL, ret, TRUE, "チャプター",        FALSE);
        move_temp_file(NULL, chap_apple, NULL, ret, TRUE, "チャプター(Apple)", FALSE);
    }
    //ステータスファイル
    if (conf->enc.use_auto_npass && sys_dat->exstg->s_local.auto_del_stats) {
        char stats[MAX_PATH_LEN];
        strcpy_s(stats, sizeof(stats), conf->vid.stats);
        cmd_replace(stats, sizeof(stats), pe, sys_dat, conf, oip);
        move_temp_file(NULL, stats, NULL, ret, TRUE, "ステータス", FALSE);
        strcat_s(stats, sizeof(stats), ".mbtree");
        move_temp_file(NULL, stats, NULL, ret, TRUE, "mbtree ステータス", FALSE);
    }
    */
    //音声ファイル(wav)
    if (strcmp(pe->append.aud[0], pe->append.wav)) //「wav出力」ならここでは処理せず下のエンコード後ファイルとして扱う
        move_temp_file(pe->append.wav,  pe->temp_filename, oip->savefile, ret, TRUE, "wav", FALSE);
    //音声ファイル(エンコード後ファイル)
    char aud_tempfile[MAX_PATH_LEN];
    PathCombineLong(aud_tempfile, _countof(aud_tempfile), pe->aud_temp_dir, PathFindFileName(pe->temp_filename));
    for (int i_aud = 0; i_aud < pe->aud_count; i_aud++)
        if (!move_temp_file(pe->append.aud[i_aud], aud_tempfile, oip->savefile, ret, !conf->oth.out_audio_only && pe->muxer_to_be_used != MUXER_DISABLED, "音声", conf->oth.out_audio_only))
            ret |= AUO_RESULT_ERROR;
    return ret;
}

DWORD GetExePriority(DWORD set, HANDLE h_aviutl) {
    if (set == AVIUTLSYNC_PRIORITY_CLASS)
        return (h_aviutl) ? GetPriorityClass(h_aviutl) : NORMAL_PRIORITY_CLASS;
    else
        return priority_table[set].value;
}

int check_video_ouput(const CONF_GUIEX *conf, const OUTPUT_INFO *oip) {
    if ((oip->flag & OUTPUT_INFO_FLAG_VIDEO) && !conf->oth.out_audio_only) {
        //if (check_ext(oip->savefile, ".mp4"))  return VIDEO_OUTPUT_MP4;
        //if (check_ext(oip->savefile, ".mkv"))  return VIDEO_OUTPUT_MKV;
        //if (check_ext(oip->savefile, ".mpg"))  return VIDEO_OUTPUT_MPEG2;
        //if (check_ext(oip->savefile, ".mpeg")) return VIDEO_OUTPUT_MPEG2;
        return VIDEO_OUTPUT_RAW;
    }
    return VIDEO_OUTPUT_DISABLED;
}

int check_muxer_to_be_used(const CONF_GUIEX *conf, int video_output_type, BOOL audio_output) {
    //if (conf->vid.afs)
    //	conf->mux.disable_mp4ext = conf->mux.disable_mkvext = FALSE; //afsなら外部muxerを強制する

    //音声なし、afsなしならmuxしない
    if (!audio_output && !conf->vid.afs)
        return MUXER_DISABLED;

    if (video_output_type == VIDEO_OUTPUT_MP4 && !conf->mux.disable_mp4ext)
        return (conf->vid.afs) ? MUXER_TC2MP4 : MUXER_MP4;
    else if (video_output_type == VIDEO_OUTPUT_MKV && !conf->mux.disable_mkvext)
        return MUXER_MKV;
    else if (video_output_type == VIDEO_OUTPUT_MPEG2 && !conf->mux.disable_mpgext)
        return MUXER_MPG;
    else
        return MUXER_DISABLED;
}

AUO_RESULT getLogFilePath(char *log_file_path, size_t nSize, const PRM_ENC *pe, const SYSTEM_DATA *sys_dat, const CONF_GUIEX *conf, const OUTPUT_INFO *oip) {
    AUO_RESULT ret = AUO_RESULT_SUCCESS;
    guiEx_settings stg(TRUE); //ログウィンドウの保存先設定は最新のものを使用する
    stg.load_log_win();
    switch (stg.s_log.auto_save_log_mode) {
        case AUTO_SAVE_LOG_CUSTOM:
            char log_file_dir[MAX_PATH_LEN];
            strcpy_s(log_file_path, nSize, stg.s_log.auto_save_log_path);
            cmd_replace(log_file_path, nSize, pe, sys_dat, conf, oip);
            PathGetDirectory(log_file_dir, _countof(log_file_dir), log_file_path);
            if (DirectoryExistsOrCreate(log_file_dir))
                break;
            ret = AUO_RESULT_WARNING;
            //下へフォールスルー
        case AUTO_SAVE_LOG_OUTPUT_DIR:
        default:
            apply_appendix(log_file_path, nSize, oip->savefile, "_log.txt");
            break;
    }
    return ret;
}

double get_duration(const OUTPUT_INFO *oip, const PRM_ENC *pe) {
    //Aviutlから再生時間情報を取得
    return ((double)(oip->n + pe->delay_cut_additional_vframe) * (double)oip->scale) / (double)oip->rate;
}

int ReadLogExe(PIPE_SET *pipes, const char *exename, LOG_CACHE *log_line_cache) {
    DWORD pipe_read = 0;
    if (pipes->stdOut.h_read) {
        if (!PeekNamedPipe(pipes->stdOut.h_read, NULL, 0, NULL, &pipe_read, NULL))
            return -1;
        if (pipe_read) {
            ReadFile(pipes->stdOut.h_read, pipes->read_buf + pipes->buf_len, sizeof(pipes->read_buf) - pipes->buf_len - 1, &pipe_read, NULL);
            pipes->buf_len += pipe_read;
            pipes->read_buf[pipes->buf_len] = '\0';
            write_log_exe_mes(pipes->read_buf, &pipes->buf_len, exename, log_line_cache);
        }
    }
    return (int)pipe_read;
}

void write_cached_lines(int log_level, const char *exename, LOG_CACHE *log_line_cache) {
    static const char *const LOG_LEVEL_STR[] = { "info", "warning", "error" };
    static const char *MESSAGE_FORMAT = "%s [%s]: %s";
    char *buffer = NULL;
    int buffer_len = 0;
    const int log_level_idx = clamp(log_level, LOG_INFO, LOG_ERROR);
    const int additional_length = strlen(exename) + strlen(LOG_LEVEL_STR[log_level_idx]) + strlen(MESSAGE_FORMAT) - strlen("%s") * 3 + 1;
    for (int i = 0; i < log_line_cache->idx; i++) {
        const int required_buffer_len = strlen(log_line_cache->lines[i]) + additional_length;
        if (buffer_len < required_buffer_len) {
            if (buffer) free(buffer);
            buffer = (char *)malloc(required_buffer_len * sizeof(buffer[0]));
            buffer_len = required_buffer_len;
        }
        if (buffer) {
            sprintf_s(buffer, buffer_len, MESSAGE_FORMAT, exename, LOG_LEVEL_STR[log_level_idx], log_line_cache->lines[i]);
            write_log_line(log_level, buffer, true);
        }
    }
    if (buffer) free(buffer);
}
#include <tlhelp32.h>

static bool check_parent(size_t check_pid, const size_t target_pid, const std::unordered_map<size_t, size_t>& map_pid) {
    for (size_t i = 0; i < map_pid.size(); i++) { // 最大でもmap_pid.size()を超えてチェックする必要はないはず
        if (check_pid == target_pid) return true;
        if (check_pid == 0) return false;
        auto key = map_pid.find(check_pid);
        if (key == map_pid.end() || key->second == 0 || key->second == key->first) return false;
        check_pid = key->second;
    }
    return false;
};

std::vector<size_t> createChildProcessIDList(const size_t target_pid) {
    auto h = unique_handle(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0), [](HANDLE h) { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); });
    if (h.get() == INVALID_HANDLE_VALUE) {
        return std::vector<size_t>();
    }

    PROCESSENTRY32 pe = { 0 };
    pe.dwSize = sizeof(PROCESSENTRY32);

    std::unordered_map<size_t, size_t> map_pid;
    if (Process32First(h.get(), &pe)) {
        do {
            map_pid[pe.th32ProcessID] = pe.th32ParentProcessID;
        } while (Process32Next(h.get(), &pe));
    }

    std::vector<size_t> list_childs;
    for (auto& [pid, parentpid] : map_pid) {
        if (check_parent(parentpid, target_pid, map_pid)) {
            list_childs.push_back(pid);
        }
    }
    return list_childs;
}

// ----------------------------------------------------------------------------------------------------------------

#include <winternl.h>

typedef __kernel_entry NTSYSCALLAPI NTSTATUS(NTAPI *NtQueryObject_t)(HANDLE Handle, OBJECT_INFORMATION_CLASS ObjectInformationClass, PVOID ObjectInformation, ULONG ObjectInformationLength, PULONG ReturnLength);
typedef __kernel_entry NTSTATUS(NTAPI *NtQuerySystemInformation_t)(SYSTEM_INFORMATION_CLASS SystemInformationClass, PVOID SystemInformation, ULONG SystemInformationLength, PULONG ReturnLength);

typedef struct _SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX {
    PVOID Object;
    ULONG_PTR UniqueProcessId;
    ULONG_PTR HandleValue;
    ULONG GrantedAccess;
    USHORT CreatorBackTraceIndex;
    USHORT ObjectTypeIndex;
    ULONG HandleAttributes;
    ULONG Reserved;
} SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX, *PSYSTEM_HANDLE_TABLE_ENTRY_INFO_EX;

typedef struct _SYSTEM_HANDLE_INFORMATION_EX {
    ULONG_PTR  NumberOfHandles;
    ULONG_PTR  Reserved;
    SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX Handles[1];
} SYSTEM_HANDLE_INFORMATION_EX, * PSYSTEM_HANDLE_INFORMATION_EX;

#pragma warning(push)
#pragma warning(disable: 4200) //C4200: 非標準の拡張機能が使用されています: 構造体または共用体中にサイズが 0 の配列があります。
typedef struct _OBJECT_NAME_INFORMATION {
    UNICODE_STRING          Name;
    WCHAR                   NameBuffer[0];
} OBJECT_NAME_INFORMATION, * POBJECT_NAME_INFORMATION;
#pragma warning(pop)

typedef struct _OBJECT_BASIC_INFORMATION {
    ULONG Attributes;
    ACCESS_MASK GrantedAccess;
    ULONG HandleCount;
    ULONG PointerCount;
    ULONG PagedPoolCharge;
    ULONG NonPagedPoolCharge;
    ULONG Reserved[3];
    ULONG NameInfoSize;
    ULONG TypeInfoSize;
    ULONG SecurityDescriptorSize;
    LARGE_INTEGER CreationTime;
} OBJECT_BASIC_INFORMATION, *POBJECT_BASIC_INFORMATION;

typedef struct _OBJECT_TYPE_INFORMATION {
    UNICODE_STRING TypeName;
    ULONG TotalNumberOfObjects;
    ULONG TotalNumberOfHandles;
    ULONG TotalPagedPoolUsage;
    ULONG TotalNonPagedPoolUsage;
    ULONG TotalNamePoolUsage;
    ULONG TotalHandleTableUsage;
    ULONG HighWaterNumberOfObjects;
    ULONG HighWaterNumberOfHandles;
    ULONG HighWaterPagedPoolUsage;
    ULONG HighWaterNonPagedPoolUsage;
    ULONG HighWaterNamePoolUsage;
    ULONG HighWaterHandleTableUsage;
    ULONG InvalidAttributes;
    GENERIC_MAPPING GenericMapping;
    ULONG ValidAccessMask;
    BOOLEAN SecurityRequired;
    BOOLEAN MaintainHandleCount;
    UCHAR TypeIndex; // since WINBLUE
    CHAR ReservedByte;
    ULONG PoolType;
    ULONG DefaultPagedPoolCharge;
    ULONG DefaultNonPagedPoolCharge;
} OBJECT_TYPE_INFORMATION, *POBJECT_TYPE_INFORMATION;

typedef struct _OBJECT_TYPES_INFORMATION {
    ULONG NumberOfTypes;
} OBJECT_TYPES_INFORMATION, *POBJECT_TYPES_INFORMATION;

#ifndef STATUS_INFO_LENGTH_MISMATCH
#define STATUS_INFO_LENGTH_MISMATCH ((NTSTATUS)0xC0000004L)
#endif

#define CEIL_INT(x, div) (((x + div - 1) / div) * div)

static std::vector<unique_handle> createProcessHandleList(const std::vector<size_t>& list_pid, const wchar_t *handle_type) {
    std::vector<unique_handle> handle_list;
    std::unique_ptr<std::remove_pointer<HMODULE>::type, decltype(&FreeLibrary)> hNtDll(LoadLibrary(_T("ntdll.dll")), FreeLibrary);
    if (hNtDll == NULL) return handle_list;

    auto fNtQueryObject = (decltype(NtQueryObject) *)GetProcAddress(hNtDll.get(), "NtQueryObject");
    auto fNtQuerySystemInformation = (decltype(NtQuerySystemInformation) *)GetProcAddress(hNtDll.get(), "NtQuerySystemInformation");
    if (fNtQueryObject == nullptr || fNtQuerySystemInformation == nullptr) {
        return handle_list;
    }

    //auto getObjectTypeNumber = [fNtQueryObject](wchar_t * TypeName) {
    //    static const auto ObjectTypesInformation = (OBJECT_INFORMATION_CLASS)3;
    //    std::vector<char> data(1024, 0);
    //    NTSTATUS status = STATUS_INFO_LENGTH_MISMATCH;
    //    do {
    //        data.resize(data.size() * 2);
    //        ULONG size = 0;
    //        status = fNtQueryObject(NULL, ObjectTypesInformation, data.data(), data.size(), &size);
    //    } while (status == STATUS_INFO_LENGTH_MISMATCH);

    //    POBJECT_TYPES_INFORMATION objectTypes = (POBJECT_TYPES_INFORMATION)data.data();
    //    char *ptr = data.data() + CEIL_INT(sizeof(OBJECT_TYPES_INFORMATION), sizeof(ULONG_PTR));
    //    for (size_t i = 0; i < objectTypes->NumberOfTypes; i++) {
    //        POBJECT_TYPE_INFORMATION objectType = (POBJECT_TYPE_INFORMATION)ptr;
    //        if (wcsicmp(objectType->TypeName.Buffer, TypeName) == 0) {
    //            return (int)objectType->TypeIndex;
    //        }
    //        ptr += sizeof(OBJECT_TYPE_INFORMATION) + CEIL_INT(objectType->TypeName.MaximumLength, sizeof(ULONG_PTR));
    //    }
    //    return -1;
    //};
    //const int fileObjectTypeIndex = getObjectTypeNumber(L"File");

    static const SYSTEM_INFORMATION_CLASS SystemExtendedHandleInformation = (SYSTEM_INFORMATION_CLASS)0x40;
    ULONG size = 0;
    fNtQuerySystemInformation(SystemExtendedHandleInformation, NULL, 0, &size);
    std::vector<char> shibuffer;
    NTSTATUS status = STATUS_INFO_LENGTH_MISMATCH;
    do {
        shibuffer.resize(size + 16*1024);
        status = fNtQuerySystemInformation(SystemExtendedHandleInformation, shibuffer.data(), shibuffer.size(), &size);
    } while (status == STATUS_INFO_LENGTH_MISMATCH);

    if (NT_SUCCESS(status)) {
        const auto currentPID = GetCurrentProcessId();
        const auto currentProcessHandle = GetCurrentProcess();
        const auto shi = (PSYSTEM_HANDLE_INFORMATION_EX)shibuffer.data();
        for (decltype(shi->NumberOfHandles) i = 0; i < shi->NumberOfHandles; i++) {
            const auto handlePID = shi->Handles[i].UniqueProcessId;
            if (std::find(list_pid.begin(), list_pid.end(), handlePID) != list_pid.end()) {
                auto handle = unique_handle((HANDLE)shi->Handles[i].HandleValue, []([[maybe_unused]] HANDLE h) { /*Do nothing*/ });
                // handleValue はプロセスごとに存在する
                // 自プロセスでなければ、DuplicateHandle で自プロセスでの調査用のhandleをつくる
                // その場合は新たに作ったhandleなので CloseHandle が必要
                if (shi->Handles[i].UniqueProcessId != currentPID) {
                    const auto hProcess = std::unique_ptr<std::remove_pointer<HANDLE>::type, decltype(&CloseHandle)>(OpenProcess(PROCESS_DUP_HANDLE, FALSE, handlePID), CloseHandle);
                    if (hProcess) {
                        HANDLE handleDup = NULL;
                        const BOOL ret = DuplicateHandle(hProcess.get(), (HANDLE)shi->Handles[i].HandleValue, currentProcessHandle, &handleDup, 0, FALSE, DUPLICATE_SAME_ACCESS);
                        if (ret) {
                            handle = unique_handle((HANDLE)handleDup, [](HANDLE h) { CloseHandle(h); });
                        }
                    }
                }
                if (handle_type) {
                    // handleの種類を確認する
                    status = fNtQueryObject(handle.get(), ObjectTypeInformation, NULL, 0, &size);
                    std::vector<char> otibuffer(size, 0);
                    status = fNtQueryObject(handle.get(), ObjectTypeInformation, otibuffer.data(), otibuffer.size(), &size);
                    const auto oti = (PPUBLIC_OBJECT_TYPE_INFORMATION)otibuffer.data();
                    if (NT_SUCCESS(status) && oti->TypeName.Buffer && _wcsicmp(oti->TypeName.Buffer, handle_type) == 0) {
                        //static const OBJECT_INFORMATION_CLASS ObjectNameInformation = (OBJECT_INFORMATION_CLASS)1;
                        //status = fNtQueryObject(handle, ObjectNameInformation, NULL, 0, &size);
                        //std::vector<char> buffer3(size, 0);
                        //status = fNtQueryObject(handle, ObjectNameInformation, buffer3.data(), buffer3.size(), &size);
                        //POBJECT_NAME_INFORMATION oni = (POBJECT_NAME_INFORMATION)buffer3.data();
                        handle_list.push_back(std::move(handle));
                    }
                } else {
                    handle_list.push_back(std::move(handle));
                }
            }
        }
    }
    return handle_list;
}

#include <filesystem>

static std::vector<std::basic_string<TCHAR>> createProcessOpenedFileList(const std::vector<size_t>& list_pid) {
    const auto list_handle = createProcessHandleList(list_pid, L"File");
    std::vector<std::basic_string<TCHAR>> list_file;
    std::vector<TCHAR> filename(32768+1, 0);
    for (const auto& handle : list_handle) {
        const auto fileType = GetFileType(handle.get());
        if (fileType == FILE_TYPE_DISK) { //ハンドルがパイプだとGetFinalPathNameByHandleがフリーズするため使用不可
            memset(filename.data(), 0, sizeof(filename[0]) * filename.size());
            auto ret = GetFinalPathNameByHandle(handle.get(), filename.data(), filename.size(), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
            if (ret != 0) {
                try {
                    auto f = std::filesystem::canonical(filename.data());
                    if (std::filesystem::is_regular_file(f)) {
                        list_file.push_back(f.string<TCHAR>());
                    }
                } catch (...) {}
            }
        }
    }
    // 重複を排除
    std::sort(list_file.begin(), list_file.end());
    auto result = std::unique(list_file.begin(), list_file.end());
    // 不要になった要素を削除
    list_file.erase(result, list_file.end());
    return list_file;
}

static bool rgy_path_is_same(const TCHAR *path1, const TCHAR *path2) {
    try {
        const auto p1 = std::filesystem::path(path1);
        const auto p2 = std::filesystem::path(path2);
        std::error_code ec;
        return std::filesystem::equivalent(p1, p2, ec);
    } catch (...) {
        return false;
    }
}

static void create_aviutl_opened_file_list(PRM_ENC *pe) {
    const auto pid_aviutl = GetCurrentProcessId();
    auto list_pid = createChildProcessIDList(pid_aviutl);
    list_pid.push_back(pid_aviutl);

    const auto list_file = createProcessOpenedFileList(list_pid);
    pe->n_opened_aviutl_files = (int)list_file.size();
    if (pe->n_opened_aviutl_files > 0) {
        pe->opened_aviutl_files = (char **)calloc(1, sizeof(char *) * pe->n_opened_aviutl_files);
        for (int i = 0; i < pe->n_opened_aviutl_files; i++) {
            pe->opened_aviutl_files[i] = _strdup(list_file[i].c_str());
        }
    }
}

static bool check_file_is_aviutl_opened_file(const char *filepath, const PRM_ENC *pe) {
    for (int i = 0; i < pe->n_opened_aviutl_files; i++) {
        if (rgy_path_is_same(filepath, pe->opened_aviutl_files[i])) {
            return true;
        }
    }
    return false;
}
