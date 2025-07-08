#include "AppleHPMLib.h"
#include <cstdio>
#include <iostream>
#include <string>
#include <sstream>
#include <unistd.h>
#include <vector>
#include <memory>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOMessage.h>
#include <IOKit/IOKitLib.h>

struct failure : public std::runtime_error {
    failure(const char *x) : std::runtime_error(x) {}
};

struct IOObjectDeleter {
    io_object_t arg;
    IOObjectDeleter(io_object_t arg) : arg(arg) {}
    ~IOObjectDeleter() { if (arg) IOObjectRelease(arg); }
};

inline void put(std::ostream &out, uint32_t val) {
    out.put(val & 0xFF);
    out.put((val >> 8) & 0xFF);
    out.put((val >> 16) & 0xFF);
    out.put((val >> 24) & 0xFF);
}

inline void put(std::ostream &out, uint8_t val) {
    out.put(val);
}

struct HPMPluginInstance {
    IOCFPlugInInterface **plugin = nullptr;
    AppleHPMLib **device;

    HPMPluginInstance(io_service_t service) {
        SInt32 score;
        IOReturn ret = IOCreatePlugInInterfaceForService(service, kAppleHPMLibType,
                                                         kIOCFPlugInInterfaceID, &plugin, &score);
        if (ret != kIOReturnSuccess)
            throw failure("IOCreatePlugInInterfaceForService failed");

        HRESULT res = (*plugin)->QueryInterface(plugin, CFUUIDGetUUIDBytes(kAppleHPMLibInterface),
                                                (LPVOID *)&device);
        if (res != S_OK)
            throw failure("QueryInterface failed");
    }

    ~HPMPluginInstance() {
        if (plugin) {
            IODestroyPlugInInterface(plugin);
        }
    }

    std::string readRegister(uint64_t chipAddr, uint8_t dataAddr, int flags = 0) {
        std::string ret(64, '\0');
        uint64_t rlen = 0;
        IOReturn x = (*device)->Read(device, chipAddr, dataAddr, &ret[0], 64, flags, &rlen);
        if (x != 0)
            throw failure("readRegister failed");
        return ret;
    }

    void writeRegister(uint64_t chipAddr, uint8_t dataAddr, std::string value) {
        IOReturn x = (*device)->Write(device, chipAddr, dataAddr, &value[0], value.length(), 0);
        if (x != 0)
            throw failure("writeRegister failed");
    }

    int command(uint64_t chipAddr, uint32_t cmd, std::string args = "") {
        if (!args.empty())
            (*device)->Write(device, chipAddr, 9, args.data(), args.length(), 0);
        auto ret = (*device)->Command(device, chipAddr, cmd, 0);
        if (ret)
            return -1;
        auto res = this->readRegister(chipAddr, 9);
        printf("Command 0x%08x result: ", cmd);
        for (int i = 0; i < 8; ++i) printf("%02x ", res[i]);
        printf("\n");
        return res[0] & 0xfu;
    }
};

std::unique_ptr<HPMPluginInstance> FindDevice() {
    CFMutableDictionaryRef matching = IOServiceMatching("AppleHPM");
    if (!matching)
        throw failure("IOServiceMatching failed");

    io_iterator_t iter = 0;
    IOObjectDeleter iterDel(iter);
    if (IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iter) != kIOReturnSuccess)
        throw failure("IOServiceGetMatchingServices failed");

    io_service_t device;
    while ((device = IOIteratorNext(iter))) {
        IOObjectDeleter deviceDel(device);

        CFNumberRef data;
        int32_t rid;
        data = (CFNumberRef)IORegistryEntryCreateCFProperty(device, CFSTR("RID"), kCFAllocatorDefault, 0);
        if (!data)
            continue;
        CFNumberGetValue(data, kCFNumberSInt32Type, &rid);
        CFRelease(data);

        if (rid != 0)
            continue;

        try {
            auto inst = std::make_unique<HPMPluginInstance>(device);
            auto reg = inst->readRegister(0, 0x3f);
            if (!(reg[0] & 1)) continue; // not connected

            io_string_t pathName;
            if (IORegistryEntryGetPath(device, kIOServicePlane, pathName) == kIOReturnSuccess)
                printf("Apple Thunderbolt Controller: %s\n", pathName);

            return inst;
        } catch (...) {
            continue;
        }
    }
    return nullptr;
}

void EnterDFUMode(HPMPluginInstance &inst) {
    printf("🔐 Entering DBMa...\n");
    bool entered = false;
    for (int attempt = 0; attempt < 10; ++attempt) {
        inst.command(0, 'DBMa');
        usleep(300000); // 300ms
        auto mode = inst.readRegister(0, 3);
        if (mode.substr(0, 4) == "DBMa") {
            printf("✅ Entered DBMa mode.\n");
            entered = true;
            break;
        }
    }

    if (!entered) {
        auto mode = inst.readRegister(0, 3);
        printf("❌ Failed to enter DBMa mode after retries. 0x03 = %02x %02x %02x %02x\n",
               mode[0], mode[1], mode[2], mode[3]);
        return;
    }

    std::vector<uint32_t> dfu{0x5ac8012, 0x106, 0x80010000};
    std::stringstream args;
    put(args, (uint8_t)((3 << 4) | dfu.size()));
    for (auto val : dfu) put(args, val);

    printf("📤 Sending DFU VDM...\n");
    int res = inst.command(0, 'VDMs', args.str());

    auto reply = inst.readRegister(0, 0x4d);
    printf("📩 DFU VDM reply (0x4d): ");
    for (int i = 0; i < 8; ++i) printf("%02x ", reply[i]);
    printf("\n");

    if (res == 0) {
        printf("✅ DFU command sent. Device should re-enumerate.\n");
    } else {
        printf("❌ DFU command failed with result code: %d\n", res);
    }
}

int main() {
    printf("Auto DFU Running...\n");
    bool waitingShown = false;

    while (true) {
        try {
            auto inst = FindDevice();
            if (!inst) {
                if (!waitingShown) {
                    printf("🔍 Waiting for Intel T2/Apple Silicon Mac...\n");
                    waitingShown = true;
                }
                sleep(1);
                continue;
            }

            waitingShown = false;

            printf("🔌 Device detected. Initiating DFU procedure...\n");
            EnterDFUMode(*inst);

            printf("🔁 Monitoring for disconnect...\n");
            for (;;) {
                try {
                    auto status = inst->readRegister(0, 0x3f);
                    if (!(status[0] & 1)) break;
                } catch (...) {
                    break;
                }
                usleep(500000);
            }
            printf("❎ Device disconnected.\n");
        } catch (const std::exception &e) {
            fprintf(stderr, "\nError: %s\n", e.what());
            sleep(2);
        }
    }

    return 0;
}
