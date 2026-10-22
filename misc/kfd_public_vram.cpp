// kfd-public-vram: check whether VRAM allocations with the KFD public flag all
// return EINVAL.
//
// Host access here is the KFD public flag, KFD_IOC_ALLOC_MEM_FLAGS_PUBLIC, on
// an AMDKFD_IOC_ALLOC_MEMORY_OF_GPU allocation whose memory type is VRAM. That
// is the flag runtimes set when CPU load/store should land in VRAM through the
// PCI BAR. Each request below is issued only after the same size and the same
// attribute flags succeed without PUBLIC, so an EINVAL is not a rejected size
// or an unsupported attribute.
//
// GTT with PUBLIC is printed once as a contrast. It is not VRAM, and it is not
// part of the count. The SVM ioctl is not used: a disabled SVM interface
// returns EINVAL for every attribute, which does not describe this allocation.
//
// Startup also prints each assigned PCI BAR from the device's sysfs resource
// file: index, type, CPU address range, and size. BAR 0 is the VRAM window.
//
// KFD_IOC_ALLOC_MEM_FLAGS_CONTIGUOUS arrived in KFD ioctl 1.16. When the
// headers used to build this program do not define it, the upstream value
// (1 << 23) is used so the binary still builds. Attribute sets that include
// that flag run only when the kernel reports ioctl 1.16 or newer. Older KFD
// skips those sets; the other sets still run.
//
// Exit status:
//   0  every counted host-access VRAM allocation returned EINVAL
//   1  a counted allocation succeeded or returned some other errno
//   2  the check could not be run

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <hip/hip_runtime_api.h>
#include <linux/kfd_ioctl.h>

// Upstream value since KFD ioctl 1.16. Older linux/kfd_ioctl.h copies omit it.
#ifndef KFD_IOC_ALLOC_MEM_FLAGS_CONTIGUOUS
#define KFD_IOC_ALLOC_MEM_FLAGS_CONTIGUOUS (1u << 23)
#define KFD_PUBLIC_VRAM_CONTIGUOUS_FROM_FALLBACK 1
#else
#define KFD_PUBLIC_VRAM_CONTIGUOUS_FROM_FALLBACK 0
#endif

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <dirent.h>
#include <fcntl.h>

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kVram = KFD_IOC_ALLOC_MEM_FLAGS_VRAM;
constexpr uint32_t kGtt = KFD_IOC_ALLOC_MEM_FLAGS_GTT;
constexpr uint32_t kWritable = KFD_IOC_ALLOC_MEM_FLAGS_WRITABLE;
constexpr uint32_t kPublic = KFD_IOC_ALLOC_MEM_FLAGS_PUBLIC;
constexpr uint32_t kExecutable = KFD_IOC_ALLOC_MEM_FLAGS_EXECUTABLE;
constexpr uint32_t kNoSubstitute = KFD_IOC_ALLOC_MEM_FLAGS_NO_SUBSTITUTE;
constexpr uint32_t kAqlQueue = KFD_IOC_ALLOC_MEM_FLAGS_AQL_QUEUE_MEM;
constexpr uint32_t kCoherent = KFD_IOC_ALLOC_MEM_FLAGS_COHERENT;
constexpr uint32_t kUncached = KFD_IOC_ALLOC_MEM_FLAGS_UNCACHED;
constexpr uint32_t kExtCoherent = KFD_IOC_ALLOC_MEM_FLAGS_EXT_COHERENT;
constexpr uint32_t kContiguous = KFD_IOC_ALLOC_MEM_FLAGS_CONTIGUOUS;
constexpr uint32_t kContiguousIoctlMajor = 1;
constexpr uint32_t kContiguousIoctlMinor = 16;

struct AttrSet {
    const char* name;
    uint32_t attr;
};

// Attribute bits that a VRAM allocation accepts without PUBLIC. PUBLIC is added
// on the host-access attempt. read-only omits WRITABLE on purpose.
constexpr AttrSet kAttrSets[] = {
    {"writable", kWritable},
    {"read-only", 0},
    {"coherent", kWritable | kCoherent},
    {"uncached", kWritable | kUncached},
    {"ext-coherent", kWritable | kExtCoherent},
    {"contiguous", kWritable | kContiguous},
    {"executable", kWritable | kExecutable},
    {"no-substitute", kWritable | kNoSubstitute},
    {"aql-queue", kWritable | kAqlQueue},
    {"coherent+contiguous", kWritable | kCoherent | kContiguous},
    {"coherent+uncached", kWritable | kCoherent | kUncached},
    {"coherent+ext-coherent", kWritable | kCoherent | kExtCoherent},
    {"executable+coherent", kWritable | kExecutable | kCoherent},
    {"no-substitute+contiguous", kWritable | kNoSubstitute | kContiguous},
    {"aql+coherent", kWritable | kAqlQueue | kCoherent},
};

struct Node {
    uint32_t gpu_id = 0;
    uint32_t domain = 0;
    uint32_t location_id = 0;
    int render_minor = -1;
    std::string pci;
};

struct AllocResult {
    bool issued = false;
    bool allocated = false;
    int err = 0;
    bool cpu_mapped = false;
    int map_err = 0;
    bool freed = true;
    int free_err = 0;
};

struct Tally {
    int counted = 0;
    int einval = 0;
    int other = 0;
    int allocated = 0;
    int not_counted = 0;
};

std::string format_bytes(uint64_t n) {
    constexpr uint64_t kKib = 1024ull;
    constexpr uint64_t kMib = 1024ull * 1024;
    constexpr uint64_t kGib = 1024ull * 1024 * 1024;
    char buf[80];
    if (n >= kGib && n % kGib == 0) {
        std::snprintf(buf, sizeof buf, "%llu GiB", static_cast<unsigned long long>(n / kGib));
    } else if (n >= kMib && n % kMib == 0) {
        std::snprintf(buf, sizeof buf, "%llu MiB", static_cast<unsigned long long>(n / kMib));
    } else if (n >= kMib && n % kKib == 0) {
        std::snprintf(buf, sizeof buf, "%llu MiB + %llu KiB",
                      static_cast<unsigned long long>(n / kMib),
                      static_cast<unsigned long long>((n % kMib) / kKib));
    } else if (n % kKib == 0) {
        std::snprintf(buf, sizeof buf, "%llu KiB", static_cast<unsigned long long>(n / kKib));
    } else {
        std::snprintf(buf, sizeof buf, "%llu bytes", static_cast<unsigned long long>(n));
    }
    return buf;
}

std::string lower_compact(std::string s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (c == ' ' || c == '-' || c == '_') {
            continue;
        }
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        const unsigned char ca = static_cast<unsigned char>(a[i]);
        const unsigned char cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb)) {
            return false;
        }
    }
    return true;
}

bool is_rx7900xtx(const std::string& name) {
    return lower_compact(name).find("7900xtx") != std::string::npos;
}

bool is_card_node(const std::string& name) {
    if (name.size() < 5 || name.compare(0, 4, "card") != 0) {
        return false;
    }
    for (size_t i = 4; i < name.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(name[i]))) {
            return false;
        }
    }
    return true;
}

bool parse_u64(const char* s, uint64_t* out) {
    if (s == nullptr || *s == '\0') {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0') {
        return false;
    }
    *out = static_cast<uint64_t>(v);
    return true;
}

bool read_u64_file(const std::string& path, uint64_t* out) {
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    uint64_t v = 0;
    in >> v;
    if (!in) {
        return false;
    }
    *out = v;
    return true;
}

std::string errno_text(int err) {
    if (err == 0) {
        return "OK";
    }
    if (err == EINVAL) {
        return "EINVAL";
    }
    char buf[160];
    std::snprintf(buf, sizeof buf, "errno %d (%s)", err, std::strerror(err));
    return buf;
}

// Bold bright red on a terminal. Piped stderr and NO_COLOR stay plain.
bool stderr_wants_color() {
    return ::isatty(STDERR_FILENO) == 1 && std::getenv("NO_COLOR") == nullptr;
}

void stderr_vfprintf(const char* fmt, va_list ap) {
    const bool color = stderr_wants_color();
    if (color) {
        std::fputs("\033[1;91m", stderr);
    }
    std::vfprintf(stderr, fmt, ap);
    if (color) {
        std::fputs("\033[0m", stderr);
    }
}

void fail_fprintf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    stderr_vfprintf(fmt, ap);
    va_end(ap);
}

void warn_fprintf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    stderr_vfprintf(fmt, ap);
    va_end(ap);
}

bool uses_contiguous(uint32_t attr) {
    return (attr & kContiguous) != 0;
}

bool contiguous_vram_supported(uint32_t major, uint32_t minor) {
    if (major > kContiguousIoctlMajor) {
        return true;
    }
    return major == kContiguousIoctlMajor && minor >= kContiguousIoctlMinor;
}

template <typename T>
int ioctl_retry(int fd, unsigned long request, T* arg) {
    for (;;) {
        const int rc = ::ioctl(fd, request, arg);
        if (rc == -1 && errno == EINTR) {
            continue;
        }
        return rc;
    }
}

std::string pci_from_location(uint32_t domain, uint32_t location_id) {
    const unsigned bus = (location_id >> 8) & 0xffu;
    const unsigned dev = (location_id >> 3) & 0x1fu;
    const unsigned func = location_id & 0x7u;
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04x:%02x:%02x.%x", domain, bus, dev, func);
    return buf;
}

bool read_node_properties(const std::string& path, Node* node) {
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    std::string key;
    std::string value;
    bool any = false;
    while (in >> key >> value) {
        any = true;
        uint64_t v = 0;
        if (!parse_u64(value.c_str(), &v)) {
            continue;
        }
        if (key == "domain") {
            node->domain = static_cast<uint32_t>(v);
        } else if (key == "location_id") {
            node->location_id = static_cast<uint32_t>(v);
        } else if (key == "drm_render_minor") {
            node->render_minor = static_cast<int>(v);
        }
    }
    return any;
}

std::vector<Node> kfd_nodes() {
    std::vector<Node> nodes;
    const char* root = "/sys/class/kfd/kfd/topology/nodes";
    DIR* dir = opendir(root);
    if (dir == nullptr) {
        return nodes;
    }
    while (dirent* de = readdir(dir)) {
        const std::string name = de->d_name;
        if (name == "." || name == "..") {
            continue;
        }
        const std::string base = std::string(root) + "/" + name;
        uint64_t gpu_id = 0;
        if (!read_u64_file(base + "/gpu_id", &gpu_id) || gpu_id == 0) {
            continue;
        }
        Node node;
        node.gpu_id = static_cast<uint32_t>(gpu_id);
        if (!read_node_properties(base + "/properties", &node) || node.render_minor < 0) {
            continue;
        }
        node.pci = pci_from_location(node.domain, node.location_id);
        nodes.push_back(std::move(node));
    }
    closedir(dir);
    std::sort(nodes.begin(), nodes.end(), [](const Node& a, const Node& b) {
        return a.gpu_id < b.gpu_id;
    });
    return nodes;
}

std::string drm_sysfs_for_pci(const std::string& pci) {
    DIR* dir = opendir("/sys/class/drm");
    if (dir == nullptr) {
        return {};
    }
    std::string found;
    const std::string key = "PCI_SLOT_NAME=";
    while (dirent* de = readdir(dir)) {
        const std::string name = de->d_name;
        if (!is_card_node(name)) {
            continue;
        }
        const std::string uevent_path = std::string("/sys/class/drm/") + name + "/device/uevent";
        std::ifstream in(uevent_path);
        std::string line;
        while (std::getline(in, line)) {
            if (line.compare(0, key.size(), key) == 0 && iequals(line.substr(key.size()), pci)) {
                found = std::string("/sys/class/drm/") + name + "/device";
                break;
            }
        }
        if (!found.empty()) {
            break;
        }
    }
    closedir(dir);
    return found;
}

// linux/ioport.h flags stored in the PCI sysfs "resource" file. The low bits of
// a ROM entry carry IORESOURCE_ROM_SHADOW from linux/pci.h.
constexpr uint64_t kResourceIo = 0x100;
constexpr uint64_t kResourceMem = 0x200;
constexpr uint64_t kResourcePrefetch = 0x2000;
constexpr uint64_t kResourceMem64 = 0x100000;
constexpr uint64_t kResourceDisabled = 0x10000000;
constexpr uint64_t kResourceUnset = 0x20000000;
constexpr uint64_t kRomShadow = 0x2;

struct PciResource {
    uint64_t start = 0;
    uint64_t end = 0;
    uint64_t flags = 0;
};

bool parse_hex_u64(const std::string& text, uint64_t* out) {
    if (text.empty()) {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text.c_str(), &end, 16);
    if (errno != 0 || end == text.c_str() || *end != '\0') {
        return false;
    }
    *out = static_cast<uint64_t>(value);
    return true;
}

std::string resource_kind(uint64_t flags, bool rom) {
    if (rom) {
        return (flags & kRomShadow) != 0 ? "shadowed" : "expansion ROM";
    }
    if ((flags & kResourceIo) != 0) {
        return "I/O";
    }
    if ((flags & kResourceMem) != 0) {
        std::string kind = "memory ";
        kind += (flags & kResourceMem64) != 0 ? "64-bit" : "32-bit";
        if ((flags & kResourcePrefetch) != 0) {
            kind += " prefetchable";
        }
        return kind;
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, "flags 0x%llx", static_cast<unsigned long long>(flags));
    return buf;
}

void print_pci_bars(const std::string& sysfs) {
    if (sysfs.empty()) {
        std::printf("PCI BAR: sysfs device path unknown\n");
        return;
    }
    std::ifstream in(sysfs + "/resource");
    if (!in) {
        std::printf("PCI BAR: resource file unreadable\n");
        return;
    }
    std::vector<PciResource> resources;
    std::string start_text;
    std::string end_text;
    std::string flags_text;
    while (in >> start_text >> end_text >> flags_text) {
        PciResource resource;
        if (!parse_hex_u64(start_text, &resource.start) || !parse_hex_u64(end_text, &resource.end) ||
            !parse_hex_u64(flags_text, &resource.flags)) {
            std::printf("PCI BAR: resource file is not start/end/flags\n");
            return;
        }
        resources.push_back(resource);
    }
    if (resources.empty()) {
        std::printf("PCI BAR: resource file is empty\n");
        return;
    }

    std::printf("PCI BAR:\n");
    bool any = false;
    for (size_t i = 0; i < resources.size() && i <= 6;) {
        const PciResource& resource = resources[i];
        const bool rom = i == 6;
        const bool blank = resource.start == 0 && resource.end == 0 && resource.flags == 0;
        if (blank) {
            ++i;
            continue;
        }
        const bool mem64 = !rom && (resource.flags & kResourceMem64) != 0;
        const bool unassigned = (resource.flags & (kResourceDisabled | kResourceUnset)) != 0 || resource.end < resource.start;
        char name[16];
        if (rom) {
            std::snprintf(name, sizeof name, "ROM");
        } else {
            std::snprintf(name, sizeof name, "BAR %zu", i);
        }
        if (unassigned) {
            std::printf("  %-6s  %-28s  unassigned\n", name, resource_kind(resource.flags, rom).c_str());
        } else {
            const uint64_t size = resource.end - resource.start + 1;
            char range[80];
            std::snprintf(range, sizeof range, "0x%llx-0x%llx",
                          static_cast<unsigned long long>(resource.start),
                          static_cast<unsigned long long>(resource.end));
            std::printf("  %-6s  %-28s  %-24s  %s\n",
                        name,
                        resource_kind(resource.flags, rom).c_str(),
                        range,
                        format_bytes(size).c_str());
        }
        any = true;
        if (mem64 && i < 5) {
            i += 2;
        } else {
            ++i;
        }
    }
    if (!any) {
        std::printf("  (none assigned)\n");
    }
}

std::string read_fd_all(int fd) {
    std::string out;
    char buf[256];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            break;
        }
        out.append(buf, buf + n);
    }
    return out;
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::string cur;
    for (char c : text) {
        if (c == '\n') {
            lines.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) {
        lines.push_back(cur);
    }
    return lines;
}

bool write_all(int fd, const std::string& text) {
    size_t off = 0;
    while (off < text.size()) {
        const ssize_t n = ::write(fd, text.data() + off, text.size() - off);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        off += static_cast<size_t>(n);
    }
    return true;
}

// HIP is queried in a child so this process does not keep the GPU VM.
// Opening /dev/kfd here after HIP has acquired the VM returns EBUSY.
bool hip_identify(std::string* pci, std::string* name, bool* preferred) {
    int fds[2] = {-1, -1};
    if (::pipe(fds) != 0) {
        return false;
    }
    std::fflush(nullptr);
    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(fds[0]);
        ::close(fds[1]);
        return false;
    }
    if (pid == 0) {
        ::close(fds[0]);
        int count = 0;
        if (hipGetDeviceCount(&count) != hipSuccess || count <= 0) {
            ::_exit(2);
        }
        int chosen = -1;
        int xtx = -1;
        char chosen_pci[64] = {};
        char chosen_name[256] = {};
        char xtx_pci[64] = {};
        char xtx_name[256] = {};
        for (int i = 0; i < count; ++i) {
            hipDeviceProp_t prop{};
            if (hipGetDeviceProperties(&prop, i) != hipSuccess) {
                continue;
            }
            char bus[64] = {};
            if (hipDeviceGetPCIBusId(bus, static_cast<int>(sizeof bus), i) != hipSuccess || bus[0] == '\0') {
                continue;
            }
            if (chosen < 0) {
                chosen = i;
                std::snprintf(chosen_pci, sizeof chosen_pci, "%s", bus);
                std::snprintf(chosen_name, sizeof chosen_name, "%s", prop.name);
            }
            if (xtx < 0 && is_rx7900xtx(prop.name)) {
                xtx = i;
                std::snprintf(xtx_pci, sizeof xtx_pci, "%s", bus);
                std::snprintf(xtx_name, sizeof xtx_name, "%s", prop.name);
            }
        }
        const bool use_xtx = xtx >= 0;
        const int use = use_xtx ? xtx : chosen;
        if (use < 0) {
            ::_exit(2);
        }
        const std::string msg = std::string(use_xtx ? "1" : "0") + "\n" +
                                (use_xtx ? xtx_pci : chosen_pci) + "\n" +
                                (use_xtx ? xtx_name : chosen_name) + "\n";
        const bool wrote = write_all(fds[1], msg);
        ::_exit(wrote ? 0 : 2);
    }

    ::close(fds[1]);
    const std::string text = read_fd_all(fds[0]);
    ::close(fds[0]);
    int status = 0;
    if (::waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return false;
    }
    const std::vector<std::string> lines = split_lines(text);
    if (lines.size() < 3 || lines[1].empty()) {
        return false;
    }
    *preferred = lines[0] == "1";
    *pci = lines[1];
    *name = lines[2];
    return true;
}

const Node* find_pci(const std::vector<Node>& nodes, const std::string& pci) {
    for (const Node& node : nodes) {
        if (iequals(node.pci, pci)) {
            return &node;
        }
    }
    return nullptr;
}

AllocResult alloc_memory(int kfd, int drm, uint32_t gpu_id, uint64_t size, uint32_t flags, bool try_cpu_map) {
    AllocResult result;
    void* va = ::mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (va == MAP_FAILED) {
        result.err = errno;
        return result;
    }

    kfd_ioctl_alloc_memory_of_gpu_args args{};
    args.va_addr = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(va));
    args.size = size;
    args.gpu_id = gpu_id;
    args.flags = flags;

    const int rc = ioctl_retry(kfd, AMDKFD_IOC_ALLOC_MEMORY_OF_GPU, &args);
    if (rc != 0) {
        result.issued = true;
        result.err = errno;
        ::munmap(va, size);
        return result;
    }

    result.issued = true;
    result.allocated = true;
    result.freed = false;
    if (try_cpu_map) {
        void* cpu = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, drm,
                           static_cast<off_t>(args.mmap_offset));
        if (cpu == MAP_FAILED) {
            result.map_err = errno;
        } else {
            result.cpu_mapped = true;
            ::munmap(cpu, size);
        }
    }

    kfd_ioctl_free_memory_of_gpu_args free_args{};
    free_args.handle = args.handle;
    const int freed = ioctl_retry(kfd, AMDKFD_IOC_FREE_MEMORY_OF_GPU, &free_args);
    if (freed == 0) {
        result.freed = true;
    } else {
        result.free_err = errno;
    }
    ::munmap(va, size);
    return result;
}

std::string alloc_text(const AllocResult& result) {
    if (!result.issued) {
        return "not issued (" + errno_text(result.err) + ")";
    }
    if (!result.allocated) {
        return errno_text(result.err);
    }
    std::string text = "allocated";
    if (result.cpu_mapped) {
        text += ", CPU map ok";
    } else if (result.map_err != 0) {
        text += ", CPU map " + errno_text(result.map_err);
    }
    if (!result.freed) {
        text += ", free " + errno_text(result.free_err);
    }
    return text;
}

std::vector<uint64_t> allocation_sizes(uint64_t page, uint64_t total, bool have_total, uint64_t visible,
                                       bool have_visible) {
    const uint64_t seeds[] = {
        page,
        64ull << 10,
        2ull << 20,
        16ull << 20,
        64ull << 20,
        128ull << 20,
        256ull << 20,
        512ull << 20,
        1ull << 30,
    };
    std::vector<uint64_t> sizes;
    auto add = [&](uint64_t n) {
        if (n < page || n % page != 0) {
            return;
        }
        if (have_total && n > total) {
            return;
        }
        if (std::find(sizes.begin(), sizes.end(), n) != sizes.end()) {
            return;
        }
        sizes.push_back(n);
    };
    for (uint64_t seed : seeds) {
        add(seed);
    }
    if (have_visible) {
        add(visible);
        if (visible <= std::numeric_limits<uint64_t>::max() - page) {
            const uint64_t over = visible + page;
            if (!have_total || over <= total) {
                add(over);
            }
        }
    }
    std::sort(sizes.begin(), sizes.end());
    return sizes;
}

bool release_failed(const AllocResult& result, const char* what) {
    if (!result.allocated || result.freed) {
        return false;
    }
    fail_fprintf("FAIL: %s was allocated and could not be freed: %s\n", what, errno_text(result.free_err).c_str());
    return true;
}

}  // namespace

int main() {
    std::string hip_pci;
    std::string hip_name;
    bool hip_preferred = false;
    const bool have_hip = hip_identify(&hip_pci, &hip_name, &hip_preferred);

    const std::vector<Node> nodes = kfd_nodes();
    if (nodes.empty()) {
        fail_fprintf("FAIL: no KFD GPU in /sys/class/kfd/kfd/topology/nodes\n");
        return 2;
    }

    const Node* node = nullptr;
    if (have_hip) {
        node = find_pci(nodes, hip_pci);
        if (node == nullptr) {
            fail_fprintf("FAIL: HIP PCI %s is not a KFD node\n", hip_pci.c_str());
            for (const Node& item : nodes) {
                fail_fprintf("  gpu_id %u  PCI %s  renderD%d\n", item.gpu_id, item.pci.c_str(), item.render_minor);
            }
            return 2;
        }
        if (!hip_preferred) {
            std::printf("RX 7900 XTX not found; using %s\n", hip_name.c_str());
        }
        std::printf("device: %s  PCI %s  KFD gpu_id %u  renderD%d\n",
                    hip_name.c_str(),
                    node->pci.c_str(),
                    node->gpu_id,
                    node->render_minor);
    } else {
        node = &nodes.front();
        std::printf("HIP device query failed; using KFD gpu_id %u  PCI %s  renderD%d\n",
                    node->gpu_id,
                    node->pci.c_str(),
                    node->render_minor);
    }

    const std::string sysfs = drm_sysfs_for_pci(node->pci);
    uint64_t vram_total = 0;
    uint64_t vram_visible = 0;
    const bool have_total = !sysfs.empty() && read_u64_file(sysfs + "/mem_info_vram_total", &vram_total);
    const bool have_visible = !sysfs.empty() && read_u64_file(sysfs + "/mem_info_vis_vram_total", &vram_visible);
    if (have_total && have_visible) {
        std::printf("VRAM: %s total, %s CPU-visible\n",
                    format_bytes(vram_total).c_str(),
                    format_bytes(vram_visible).c_str());
    } else if (have_total) {
        std::printf("VRAM: %s total, CPU-visible size unknown\n", format_bytes(vram_total).c_str());
    } else {
        std::printf("VRAM: sysfs size unknown\n");
    }
    print_pci_bars(sysfs);

    const long page_value = ::sysconf(_SC_PAGESIZE);
    if (page_value <= 0) {
        fail_fprintf("FAIL: page size is not available\n");
        return 2;
    }
    const uint64_t page = static_cast<uint64_t>(page_value);

    const int kfd = ::open("/dev/kfd", O_RDWR | O_CLOEXEC);
    if (kfd < 0) {
        fail_fprintf("FAIL: open /dev/kfd: %s\n", std::strerror(errno));
        return 2;
    }
    const std::string render_path = "/dev/dri/renderD" + std::to_string(node->render_minor);
    const int drm = ::open(render_path.c_str(), O_RDWR | O_CLOEXEC);
    if (drm < 0) {
        fail_fprintf("FAIL: open %s: %s\n", render_path.c_str(), std::strerror(errno));
        ::close(kfd);
        return 2;
    }

    kfd_ioctl_get_version_args version{};
    if (ioctl_retry(kfd, AMDKFD_IOC_GET_VERSION, &version) != 0) {
        fail_fprintf("FAIL: AMDKFD_IOC_GET_VERSION: %s\n", std::strerror(errno));
        ::close(drm);
        ::close(kfd);
        return 2;
    }
    std::printf("KFD ioctl %u.%u\n", version.major_version, version.minor_version);
    const bool contiguous_ok = contiguous_vram_supported(version.major_version, version.minor_version);
#if KFD_PUBLIC_VRAM_CONTIGUOUS_FROM_FALLBACK
    std::printf("KFD_IOC_ALLOC_MEM_FLAGS_CONTIGUOUS: not in the build headers; using 1<<23\n");
#endif
    if (!contiguous_ok) {
        warn_fprintf(
            "WARNING: KFD ioctl %u.%u does not include contiguous VRAM allocation (added in 1.16).\n"
            "         Contiguous attribute sets are skipped.\n"
            "         A newer KFD UAPI and a matching kernel/amdgpu/KFD driver are required for those tests.\n",
            version.major_version,
            version.minor_version);
        std::printf("contiguous VRAM: skipped (need KFD ioctl %u.%u or newer)\n",
                    kContiguousIoctlMajor,
                    kContiguousIoctlMinor);
        std::printf("skipped sets:");
        for (const AttrSet& set : kAttrSets) {
            if (uses_contiguous(set.attr)) {
                std::printf(" %s", set.name);
            }
        }
        std::printf("\n");
    } else {
        std::printf("contiguous VRAM: enabled\n");
    }

    kfd_ioctl_acquire_vm_args vm{};
    vm.drm_fd = static_cast<uint32_t>(drm);
    vm.gpu_id = node->gpu_id;
    if (ioctl_retry(kfd, AMDKFD_IOC_ACQUIRE_VM, &vm) != 0) {
        fail_fprintf("FAIL: AMDKFD_IOC_ACQUIRE_VM: %s\n", std::strerror(errno));
        ::close(drm);
        ::close(kfd);
        return 2;
    }

    std::printf("\nEach host-access line is VRAM | PUBLIC plus the attributes in the name.\n");
    std::printf("It counts only when the same allocation without PUBLIC succeeds.\n\n");

    const AllocResult gtt = alloc_memory(kfd, drm, node->gpu_id, page, kGtt | kPublic | kWritable, false);
    std::printf("%-24s %18s  %s  flags=0x%08x  (GTT, not counted)\n",
                "gtt-public",
                format_bytes(page).c_str(),
                alloc_text(gtt).c_str(),
                kGtt | kPublic | kWritable);
    if (release_failed(gtt, "GTT contrast allocation")) {
        ::close(drm);
        ::close(kfd);
        return 2;
    }
    std::printf("\n");

    const std::vector<uint64_t> sizes = allocation_sizes(page, vram_total, have_total, vram_visible, have_visible);
    if (sizes.empty()) {
        fail_fprintf("FAIL: no page-aligned size is within VRAM\n");
        ::close(drm);
        ::close(kfd);
        return 2;
    }

    Tally tally;
    int contiguous_sets_skipped = 0;
    std::vector<std::string> exceptions;
    for (const AttrSet& set : kAttrSets) {
        if (!contiguous_ok && uses_contiguous(set.attr)) {
            contiguous_sets_skipped += 1;
            continue;
        }
        for (uint64_t size : sizes) {
            const uint32_t control_flags = kVram | set.attr;
            const uint32_t host_flags = control_flags | kPublic;
            const AllocResult control = alloc_memory(kfd, drm, node->gpu_id, size, control_flags, false);
            if (release_failed(control, "control allocation")) {
                ::close(drm);
                ::close(kfd);
                return 2;
            }

            const bool count = control.issued && control.allocated && control.freed;
            AllocResult host;
            if (count) {
                host = alloc_memory(kfd, drm, node->gpu_id, size, host_flags, true);
                if (release_failed(host, "host-access allocation")) {
                    ::close(drm);
                    ::close(kfd);
                    return 2;
                }
            }

            const std::string host_text = count ? alloc_text(host) : "not counted";
            std::printf("%-24s %18s  control %-16s  host-access %-16s  flags=0x%08x%s\n",
                        set.name,
                        format_bytes(size).c_str(),
                        alloc_text(control).c_str(),
                        host_text.c_str(),
                        host_flags,
                        count ? "" : "  not counted");
            std::fflush(stdout);

            if (!count) {
                tally.not_counted += 1;
                continue;
            }
            tally.counted += 1;
            if (host.allocated) {
                tally.allocated += 1;
                exceptions.push_back(std::string(set.name) + " " + format_bytes(size) + ": " + alloc_text(host));
            } else if (host.err == EINVAL) {
                tally.einval += 1;
            } else {
                tally.other += 1;
                exceptions.push_back(std::string(set.name) + " " + format_bytes(size) + ": " + errno_text(host.err));
            }
        }
    }

    ::close(drm);
    ::close(kfd);

    std::printf("\nhost-access VRAM allocations: %d\n", tally.counted);
    std::printf("EINVAL: %d\n", tally.einval);
    std::printf("other errno: %d\n", tally.other);
    std::printf("allocated: %d\n", tally.allocated);
    std::printf("not counted: %d\n", tally.not_counted);
    if (contiguous_sets_skipped > 0) {
        std::printf("contiguous sets skipped: %d\n", contiguous_sets_skipped);
    }
    if (!exceptions.empty()) {
        std::printf("did not return EINVAL:\n");
        for (const std::string& line : exceptions) {
            std::printf("  %s\n", line.c_str());
        }
    }

    if (tally.counted == 0) {
        fail_fprintf("FAIL: no host-access VRAM allocation could be counted\n");
        std::printf("verdict: setup-failed\n");
        return 2;
    }
    if (tally.einval == tally.counted) {
        std::printf("verdict: all-einval\n");
        std::printf("Every counted host-access VRAM allocation returned EINVAL (%d of %d).\n",
                    tally.einval,
                    tally.counted);
        return 0;
    }
    std::printf("verdict: not-all-einval\n");
    std::printf("Not every host-access VRAM allocation returned EINVAL (%d EINVAL, %d allocated, %d other, of %d).\n",
                tally.einval,
                tally.allocated,
                tally.other,
                tally.counted);
    return 1;
}
