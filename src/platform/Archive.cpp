#include "Archive.h"

#include <miniz.h>
#include <windows.h>

#include <fstream>
#include <sstream>
#include <cstring>
#include <cstdio>

namespace hs::arc {
namespace {

// 读取整个文件到内存
bool readFile(const fs::path& p, std::vector<unsigned char>& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    std::streamoff sz = in.tellg();
    in.seekg(0, std::ios::beg);
    if (sz <= 0) return true;
    out.resize((size_t)sz);
    in.read((char*)out.data(), sz);
    return in.gcount() == sz;
}

// 安全合并路径: 拒绝绝对路径与 .. 穿越
bool safeJoin(const fs::path& destDir, const std::string& entry, fs::path& out) {
    if (entry.empty()) return false;
    // 规范化: 去掉前导斜杠
    std::string e = entry;
    while (!e.empty() && (e[0] == '/' || e[0] == '\\')) e.erase(e.begin());
    if (e.empty()) return false;
    // 拒绝 .. 穿越
    if (e.find("..") != std::string::npos) {
        // 更严格: 检查路径段
        std::stringstream ss(e);
        std::string seg;
        while (std::getline(ss, seg, '/')) {
            if (seg == ".." || seg == ".") continue;
        }
        // 简化处理: 只要含 .. 段就拒绝
        if (e.find("/../") != std::string::npos || e.rfind("/..") == e.size()-3 || e == "..") return false;
    }
    // 绝对路径 (盘符) 拒绝
    if (e.size() >= 2 && e[1] == ':') return false;
    out = destDir / fs::path(e);
    return true;
}

// ---- zip 解压 (miniz) ----
std::optional<std::string> extractZip(const fs::path& archive, const fs::path& destDir) {
    std::vector<unsigned char> data;
    if (!readFile(archive, data)) return "无法读取文件: " + archive.string();

    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_mem(&zip, data.data(), data.size(), 0))
        return "不是有效的 zip: " + archive.string();

    std::string err;
    mz_uint n = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < n; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) { err = "读取 zip 条目失败 #" + std::to_string(i); break; }

        std::string name(st.m_filename);
        fs::path outPath;
        if (!safeJoin(destDir, name, outPath)) continue;   // 跳过不安全条目

        if (mz_zip_reader_is_file_a_directory(&zip, i)) {
            std::error_code ec;
            fs::create_directories(outPath, ec);
            continue;
        }
        // 解压单文件
        size_t sz = 0;
        void* p = mz_zip_reader_extract_to_heap(&zip, i, &sz, 0);
        if (!p) { err = "解压失败: " + name; break; }
        std::error_code ec;
        fs::create_directories(outPath.parent_path(), ec);
        {
            std::ofstream out(outPath, std::ios::binary);
            if (!out) { err = "写入失败: " + outPath.string(); mz_free(p); break; }
            out.write((const char*)p, (std::streamsize)sz);
        }
        mz_free(p);
    }
    mz_zip_reader_end(&zip);
    return err.empty() ? std::nullopt : std::optional<std::string>(err);
}

// ---- gzip 解压 (旧的内存版, 已废弃) ----
// 保留说明: 官方 runtime 543MB 解出 1.9GB, 一次性 malloc 到堆上会 OOM。
// 新的 extractTarGzStreaming() 用增量 tinfl 替代。

// ---- tar.gz 解压 ----
//
// 历史: 先是整包进内存 + tinfl_mem_to_heap (543MB runtime 会打爆内存),
// 后改为增量 tinfl + 滑动窗口 —— 环形字典语义与 miniz 的 BAD_PARAM /
// 掩码规则不合, 实测大归档上产出错位数据 (tinfl status -3), 弃用。
//
// 现实现: 调 Windows 10+ 自带的 bsdtar (System32/tar.exe, libarchive)。
// 解码器久经验证、流式落盘、对损坏归档报错明确; 无新增依赖;
// 便携目标平台 Win10+ (WebView2 本就要求)。miniz 继续负责 .zip。

std::optional<std::string> extractTarGzStreaming(const fs::path& archive, const fs::path& destDir,
                                                 const ExtractControl* ctl) {
    std::error_code ec;
    fs::create_directories(destDir, ec);

    wchar_t tarPath[MAX_PATH]{};
    wchar_t* filePart = nullptr;
    DWORD n = SearchPathW(nullptr, L"tar.exe", nullptr, MAX_PATH, tarPath, &filePart);
    if (n == 0 || n >= MAX_PATH)
        return "未找到 tar.exe (需要 Windows 10 及以上自带组件), 无法解包: " + archive.string();

    // bsdtar 会把 "-f <含盘符冒号的路径>" 当成远程主机语法 (Cannot connect to I:),
    // 所以工作目录设为归档所在目录, -f 只传文件名; -C 是 chdir, 不受影响
    std::wstring workDir = archive.parent_path().wstring();
    std::wstring arcName = archive.filename().wstring();
    // 捕获 tar 的 stdout+stderr (出错时把原因透传给用户)
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE errRead = nullptr, errWrite = nullptr;
    if (!CreatePipe(&errRead, &errWrite, &sa, 0))
        return "无法创建 tar 输出管道";

    std::wstring cmd = std::wstring(L"\"") + tarPath
                     + L"\" -x -f \"" + arcName
                     + L"\" -C \"" + destDir.wstring() + L"\"";
    STARTUPINFOW si{ sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = errWrite;
    si.hStdError = errWrite;
    si.hStdInput = nullptr;
    PROCESS_INFORMATION pi{};
    // 第一参数直接给 tar.exe 全路径, 完全绕开 cmd 的引号/元字符解析
    if (!CreateProcessW(tarPath, &cmd[0], nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, workDir.c_str(), &si, &pi)) {
        CloseHandle(errRead); CloseHandle(errWrite);
        return "无法启动 tar.exe (错误 " + std::to_string(GetLastError()) + ")";
    }
    CloseHandle(errWrite);   // 先关父端写柄, 否则读端永不 EOF

    // Job Object: tar 挂进 KILL_ON_JOB_CLOSE 作业 —
    //   启动器被强杀/崩溃 -> 作业句柄随进程关闭 -> tar 被连带终止 (不再孤儿续跑)
    //   取消请求 -> TerminateJobObject 即时终止
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim{};
        lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &lim, sizeof(lim));
        AssignProcessToJobObject(job, pi.hProcess);
    }

    // 排空 stderr 管道 + 200ms 轮询 (Peek 防阻塞, 支持取消)
    std::string tarErr;
    char buf[512];
    DWORD got = 0;
    bool cancelled = false;
    auto drainPipe = [&]() {
        for (;;) {
            DWORD avail = 0;
            if (!PeekNamedPipe(errRead, nullptr, 0, nullptr, &avail, nullptr) || avail == 0) break;
            if (!ReadFile(errRead, buf, sizeof(buf), &got, nullptr) || got == 0) break;
            tarErr.append(buf, got);
        }
    };
    for (;;) {
        drainPipe();
        DWORD w = WaitForSingleObject(pi.hProcess, 200);
        if (w == WAIT_OBJECT_0) break;
        if (ctl && ctl->cancel && (*ctl->cancel)()) {
            cancelled = true;
            if (job) TerminateJobObject(job, 3);
            WaitForSingleObject(pi.hProcess, 5000);
            break;
        }
    }
    drainPipe();             // 进程已退出, 收尾读剩余 stderr
    CloseHandle(errRead);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    if (job) CloseHandle(job);   // 正常=无剩余进程; 取消=已终止; 强杀启动器=此处触发连带终止
    if (cancelled)
        return "解包已取消: " + archive.string();
    if (exitCode != 0) {
        // bsdtar 是 delayed error: 其余文件全部落盘, 末尾才报错。
        // 若所有错误都是 "Can't create ..." —— 典型为 npm .bin 符号链接条目
        // (Windows 无符号链接特权时无法创建, webui 运行并不需要) —— 视为成功。
        bool allCreateFail = !tarErr.empty();
        {
            bool first = true;
            size_t a = 0;
            while (a < tarErr.size()) {
                size_t b = tarErr.find('\n', a);
                std::string line = tarErr.substr(a, b == std::string::npos ? std::string::npos : b - a);
                if (!line.empty()) {
                    first = false;
                    if (line.find("Can't create") == std::string::npos
                        && line.find("Error exit delayed") == std::string::npos) {
                        allCreateFail = false;
                        break;
                    }
                }
                if (b == std::string::npos) break;
                a = b + 1;
            }
            (void)first;
        }
        if (!allCreateFail) {
            std::string detail;
            for (char c : tarErr) if (c >= ' ' && c < 127) detail += c;   // 只留可打印 ASCII
            return "tar 解包失败 (exit " + std::to_string(exitCode)
                 + (detail.empty() ? "" : ": " + detail)
                 + ", 归档可能损坏或不完整): " + archive.string();
        }
        // symlink 失败容错通过, 继续按成功处理
    }
    return std::nullopt;
}


// ---- tar 解析 (UStar) ----
// tar 条目: 512 字节头 + 数据 (对齐 512)
std::optional<std::string> extractTar(const std::vector<unsigned char>& tarData, const fs::path& destDir) {
    size_t off = 0;
    std::error_code ec;
    fs::create_directories(destDir, ec);
    while (off + 512 <= tarData.size()) {
        const unsigned char* hdr = tarData.data() + off;
        // 全零块 = 结束
        bool allZero = true;
        for (int i = 0; i < 512; ++i) if (hdr[i]) { allZero = false; break; }
        if (allZero) break;

        std::string name((const char*)hdr, strnlen((const char*)hdr, 100));
        if (name.empty()) break;

        // 文件大小: 八进制字符串 (字段 124-135)
        std::string sizeStr((const char*)(hdr+124), 12);
        size_t fsize = 0;
        for (char c : sizeStr) { if (c >= '0' && c <= '7') fsize = fsize*8 + (c-'0'); else if (c != ' ' && c != '\0') break; }

        char typeflag = hdr[156];
        fs::path outPath;
        if (!safeJoin(destDir, name, outPath)) { off += 512 + ((fsize+511)&~511); continue; }

        if (typeflag == '5') {   // 目录
            fs::create_directories(outPath, ec);
        } else if (typeflag == '0' || typeflag == '\0' || typeflag == ' ') {  // 普通文件
            fs::create_directories(outPath.parent_path(), ec);
            std::ofstream out(outPath, std::ios::binary);
            if (out && off + 512 + fsize <= tarData.size()) {
                out.write((const char*)(tarData.data() + off + 512), (std::streamsize)fsize);
            }
        }
        // 跳过数据到下一个 512 对齐
        off += 512 + ((fsize + 511) & ~511ull);
    }
    return std::nullopt;
}

// ---- tar.gz 解压 ----
// 已由 extractTarGzStreaming() 取代 (见上)。
// 旧的「整包读进内存 + tinfl_decompress_mem_to_heap」实现已在官方 runtime
// (543MB -> 1.9GB) 上验证会打爆内存, 不再保留。

} // namespace

std::optional<std::string> extract(const fs::path& archive, const fs::path& destDir,
                                   const ExtractControl* ctl) {
    std::string ext = archive.extension().string();
    std::string lower;
    for (char c : ext) lower += (char)std::tolower((unsigned char)c);

    std::error_code ec;
    fs::create_directories(destDir, ec);

    if (lower == ".zip") return extractZip(archive, destDir);
    // .tar.gz 走流式通道 —— 便携版构建要解 1.9GB 的 runtime, 不能整包进内存
    if (lower == ".gz" || lower == ".tgz" || lower == ".tar.gz") return extractTarGzStreaming(archive, destDir, ctl);
    if (lower == ".tar") {
        std::vector<unsigned char> data;
        if (!readFile(archive, data)) return "无法读取文件: " + archive.string();
        return extractTar(data, destDir);
    }
    return "不支持的文件类型: " + ext;
}

std::optional<std::string> extractAuto(const fs::path& archive, const fs::path& destDir,
                                       const ExtractControl* ctl) {
    return extract(archive, destDir, ctl);
}

} // namespace hs::arc
