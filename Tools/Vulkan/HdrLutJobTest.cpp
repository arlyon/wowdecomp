#include "Engine/Source/Gx/CGxDeviceVulkan/HdrLutJob.h"
#include <chrono>
#include <iostream>
#include <stdexcept>

void Check(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
void Wait(GxVulkan::HdrLutJob& job) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(!job.Ready() && std::chrono::steady_clock::now()<deadline) std::this_thread::yield();
    Check(job.Ready(),"LUT worker timeout");
}
int main(int argc,char** argv) {
    try {
        Check(argc==2,"controlled fixture directory required");
        const std::string directory=argv[1];
        Check(GxHdrLutPathValid("") && GxHdrLutPathValid("local.cube"),"None or cube path rejected");
        Check(!GxHdrLutPathValid("x.cube\n") && !GxHdrLutPathValid("x.txt") && !GxHdrLutPathValid(std::string(1025,'x').c_str()),"invalid path admitted");
        bool bounded=false;
        try { std::istringstream oversized("TITLE \""+std::string(5000,'x')+"\"\n");GxVulkan::ReadHdrCube(oversized); }
        catch(const std::exception&) {bounded=true;}
        Check(bounded,"oversized title bypassed fixed input line bound");
        GxVulkan::HdrLutJob valid(directory+"/HdrIdentity.cube");Wait(valid);
        Check(valid.error.empty() && valid.lut.size==2 && valid.lut.rgb.size()==8,"production worker parse failed");
        Check(valid.lut.rgb.capacity()*sizeof(valid.lut.rgb[0])<GxVulkan::HdrLutJob::Reservation,"output exceeds reservation");
        GxVulkan::HdrLutJob invalid(directory+"/HdrInvalid.cube");Wait(invalid);
        Check(!invalid.error.empty() && invalid.lut.rgb.empty(),"invalid cube admitted");
        GxVulkan::HdrLutJob missing(directory+"/missing.cube");Wait(missing);
        Check(!missing.error.empty(),"missing file treated as None/success");
        std::cout<<"PASS: production owned worker, strict paths, identity cube, invalid domain and missing file; reservation="<<GxVulkan::HdrLutJob::Reservation<<"\n";
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
