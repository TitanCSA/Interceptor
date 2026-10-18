#include "../PublicAPI/Interceptor.h"
#include <cstdio>
#include <iostream>
#include <system_error>
int wmain(int argc,wchar_t** argv) {
    using namespace Interceptor;
    try {
        const bool verbose=argc==2 && std::wstring(argv[1])==L"--verbose";
        if(argc!=1&&!verbose)throw std::invalid_argument("Usage: 07_Manette_HID_Inverse_X_Y [--verbose]");
        const auto init=Init_AllDevices();
        if(!init.ready) {
            std::cerr<<"Initialization incomplete; error="<<init.error<<", restart="<<init.reboot_required<<'\n';return 1;
        }
        for(const auto& device:Get_Devices())
            std::printf("type=%u id=%u VID=%04X PID=%04X report=%u bytes\n",device.type,device.id,device.vid,device.pid,device.input_bytes);
        std::cout<<"Automatic HID X/Y inversion. Other families are forwarded. Ctrl+C or close to restore.\n";
        if(verbose)std::cout<<"Verbose sample: at most four reports/second, first 32 bytes. Console output can delay the next read.\n";
        DWORD mode=0;const auto console=GetStdHandle(STD_INPUT_HANDLE);
        if(GetConsoleMode(console,&mode))SetConsoleMode(console,(mode|ENABLE_EXTENDED_FLAGS)&~ENABLE_QUICK_EDIT_MODE);
        ULONGLONG nextSample=0;
        for(;;) {
            auto inputs=Get_Input_With_Timeout(INFINITE,nullptr);
            const Controller_Hid_Input* sample=nullptr;
            for(auto& event:inputs) {
                auto* report=event.Get<Controller_Hid_Input>();
                if(!report)continue;
                auto& input=*report;
                for(auto& axis:input.state.axes)
                    if(axis.usage_page==1 && (axis.usage==0x30 || axis.usage==0x31) && axis.normalized_valid)
                        axis.normalized=-axis.normalized;
                sample=&input;
            }
            const auto sent=Send_Input(inputs);
            if(!sent)throw std::system_error(static_cast<int>(sent.error),std::system_category(),"Send input batch");
            // Forward the complete batch before any formatting/output. Normal
            // operation performs no per-report console work; verbose is an
            // explicit bounded diagnostic sample, not a packet logger.
            if(verbose && sample && GetTickCount64()>=nextSample) {
                nextSample=GetTickCount64()+250;
                std::printf("device=%u sequence=%llu bytes=%u decode_error=%lu original report:",sample->device,sample->sequence,sample->length,sample->decode_error);
                for(std::uint32_t i=0;i<std::min<std::uint32_t>(sample->length,32);++i)std::printf(" %02X",sample->report[i]);
                if(sample->length>32)std::printf(" ...");
                std::putchar('\n');
            }
        }
    } catch(const std::exception& e) {
        std::cerr<<e.what()<<"\nCapture closed.\n";return 1;
    }
}
