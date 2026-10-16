#include "Engine/Source/Storm/H/storm.h"
#include "Engine/Source/Storm/Source/Sha1.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); std::abort(); } } while (0)

static void arc4() {
    // RFC 6229, 40-bit key, first 16 keystream bytes; deliberately unaligned.
    const uint8_t key[] = {1,2,3,4,5};
    const uint8_t expected[] = {0xb2,0x39,0x63,0x05,0xf0,0x3d,0xc0,0x27,0xcc,0xc3,0x52,0x4a,0x0a,0x11,0x18,0xa8};
    alignas(4) uint8_t data[17]{};
    SARC4Key initial, state;
    SARC4PrepareKey(key, sizeof(key), &initial);
    SARC4ProcessBuffer(data+1, 16, &initial, &state);
    CHECK(!std::memcmp(data+1, expected, 16));
    SARC4ProcessBuffer(data+1, 7, &initial, &state);
    SARC4ProcessBuffer(data+8, 9, &state, &state);
    for (int i=1;i<17;++i) CHECK(data[i]==0);
    // Widely published RC4 "Key"/"Plaintext" known answer.
    uint8_t plaintext[]="Plaintext";
    const uint8_t ciphertext[]={0xbb,0xf3,0x16,0xe8,0xd9,0x40,0xaf,0x0a,0xd3};
    SARC4PrepareKey("Key",3,&state);
    SARC4ProcessBuffer(plaintext,9,&state,&state);
    CHECK(!std::memcmp(plaintext,ciphertext,9));
}
static void hashes() {
    CHECK(SCrcCompute("123456789", 9, nullptr, SCRC_INITIALIZE|SCRC_PROCESS|SCRC_FINALIZE)==0xcbf43926);
    CHECK(SCrcCompute("",0,nullptr,SCRC_INITIALIZE|SCRC_PROCESS|SCRC_FINALIZE)==0);
    uint32_t crc;
    SCrcCompute("1234",4,&crc,SCRC_INITIALIZE|SCRC_PROCESS);
    CHECK(SCrcCompute("56789",5,&crc,SCRC_PROCESS|SCRC_FINALIZE)==0xcbf43926);
    // FIPS PUB 180-1 SHA-1 example.
    const uint8_t expected[] = {0xa9,0x99,0x3e,0x36,0x47,0x06,0x81,0x6a,0xba,0x3e,0x25,0x71,0x78,0x50,0xc2,0x6c,0x9c,0xd0,0xd8,0x9d};
    Sha1 sha; uint8_t digest[20]; sha.Initialize(); sha.Append("a",1); sha.Append("bc",2); sha.Finalize(digest);
    CHECK(!std::memcmp(digest,expected,20));
}
static uint32_t value(BigData* n) {
    uint32_t result=0, bytes; SBigToBinaryBuffer(n,&result,4,&bytes); CHECK(bytes<=4); return result;
}
static void big() {
    BigData *a,*b,*c,*d; SBigNew(&a);SBigNew(&b);SBigNew(&c);SBigNew(&d);
    SBigFromUnsigned(b,65); SBigFromUnsigned(c,17); SBigFromUnsigned(d,3233);
    SBigPowMod(a,b,c,d); CHECK(value(a)==2790); // textbook RSA known answer
    SBigFromUnsigned(c,2753); SBigPowMod(b,a,c,d); CHECK(value(b)==65);
    SBigFromUnsigned(c,17); SBigAdd(a,b,c); CHECK(value(a)==82);
    SBigSub(a,a,c); CHECK(SBigCompare(a,b)==0);
    SBigMul(a,b,c); CHECK(value(a)==1105); SBigMod(a,a,c); CHECK(value(a)==0);
    const uint8_t binary[]={0x78,0x56,0x34,0x12}; SBigFromBinary(a,binary,4); CHECK(value(a)==0x12345678);
    uint32_t bits; SBigBitLen(a,&bits); CHECK(bits==29);
    SBigDel(a);SBigDel(b);SBigDel(c);SBigDel(d);
}
static void unicode() {
    // Unicode scalar U+0041, U+00A2, U+20AC, U+1F600 (surrogate pair).
    const char utf8[]="A\xc2\xa2\xe2\x82\xac\xf0\x9f\x98\x80";
    const uint16_t utf16[]={0x41,0xa2,0x20ac,0xd83d,0xde00,0};
    uint16_t wide[16]{}; char narrow[32]{}; uint32_t read,written;
    CHECK(SUniConvertUTF8to16(wide,16,utf8,sizeof(utf8),&written,&read)==0);
    CHECK(!std::memcmp(wide,utf16,sizeof(utf16)) && written==6 && read==10);
    CHECK(SUniConvertUTF16to8(narrow,32,utf16,6,&written,&read)==0);
    CHECK(!std::strcmp(narrow,utf8) && written==11 && read==5);
    CHECK(SUniConvertUTF8to16(wide,1,utf8,sizeof(utf8),&written,&read)>0);
    CHECK(written==1 && read==1);
    CHECK(SUniConvertUTF8to16(wide,16,"\xf0\x9f",2,&written,&read)<0);
    CHECK(SUniConvertUTF16to8(narrow,32,utf16+3,1,&written,&read)<0);
}
static const uint8_t modulus[] = {0x1f,0x8b,0x73,0x38,0x29,0x31,0xb1,0x1,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0xb0,0x1b,0x1d,0x21,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x40};
static const uint8_t signature[] = {0x24,0xc2,0xf0,0x73,0x58,0xea,0x6a,0xbf,0xc1,0x9f,0x50,0xf9,0x59,0xcf,0x67,0x79,0xec,0x7a,0x42,0x4a,0xf5,0xe5,0x7f,0x26,0xc,0x62,0xe7,0x57,0xc3,0xf3,0x3f,0x24,0xb2,0x42,0xab,0x13,0x58,0xc9,0xfa,0xd6,0xaf,0xd7,0x1,0xa4,0xaf,0x2f,0xf4,0xcd,0x34,0x44,0x4f,0x6e,0x73,0x1e,0x64,0x93,0x6f,0x3c,0x35,0xaa,0xad,0x88,0x5d,0x14};
static void signatures() {
    // Generated, test-only 512-bit RSA fixture; SHA-1 + BB padding + 0B,
    // little-endian integer encoding, as required by the actual Storm format.
    const char message[]="generated Storm signature fixture";
    const uint8_t exponent[]={1,0,1};
    std::vector<uint8_t> data(message,message+sizeof(message)-1);
    const uint8_t magic[]={'N','G','I','S'};
    data.insert(data.end(),magic,magic+4);data.insert(data.end(),signature,signature+64);
    CHECK(SSignatureVerify(data.data(),data.size(),modulus,64,exponent,3)==1);
    for (unsigned chunk : {1u,7u,64u,128u}) {
        SSignatureData* token; SSignatureVerifyStream_Begin(&token,64,3);
        CHECK(SSignatureVerifyStream_GetSignatureLength(token)==68);
        for (size_t i=0;i<data.size();i+=chunk) SSignatureVerifyStream_ProvideData(token,data.data()+i,std::min<size_t>(chunk,data.size()-i));
        CHECK(SSignatureVerifyStream_Finish(token,modulus,exponent)==1);
    }
    for (size_t index : {size_t(0),size_t(sizeof(message)-1),data.size()-1}) {
        data[index]^=1; CHECK(!SSignatureVerify(data.data(),data.size(),modulus,64,exponent,3)); data[index]^=1;
    }
    uint8_t wrongModulus[64];std::memcpy(wrongModulus,modulus,64);wrongModulus[0]^=2;
    CHECK(!SSignatureVerify(data.data(),data.size(),wrongModulus,64,exponent,3));
    const uint8_t wrongExponent[]={3};
    CHECK(!SSignatureVerify(data.data(),data.size(),modulus,64,wrongExponent,1));
    CHECK(!SSignatureVerify(data.data(),10,modulus,64,exponent,3));
    data[0]^=1;
    SSignatureData* token;SSignatureVerifyStream_Begin(&token,64,3);
    for (size_t i=0;i<data.size();++i) SSignatureVerifyStream_ProvideData(token,data.data()+i,1);
    CHECK(!SSignatureVerifyStream_Finish(token,modulus,exponent));
}
static void bounds(HSRGN r,float l,float b,float right,float t) {
    RECTF out; SRgnGetBoundingRectf(r,&out); CHECK(out.left==l && out.bottom==b && out.right==right && out.top==t);
}
static void regions() {
    HSRGN r;SRgnCreate(&r,0);CHECK(r);bounds(r,0,0,0,0);
    RECTF a{1,1,5,5}, b{3,3,7,7}, param{20,20,30,30};
    SRgnCombineRectf(r,&a,nullptr,SRGN_OR);bounds(r,1,1,5,5);
    SRgnCombineRectf(r,&b,nullptr,SRGN_AND);bounds(r,3,3,5,5);
    SRgnCombineRectf(r,&b,nullptr,SRGN_DIFF);bounds(r,0,0,0,0);
    SRgnCombineRectf(r,&a,nullptr,SRGN_OR);SRgnCombineRectf(r,&b,nullptr,SRGN_OR);bounds(r,1,1,7,7);
    SRgnCombineRectf(r,&param,reinterpret_cast<void*>(1),SRGN_PARAMONLY);bounds(r,1,1,7,7);
    SRgnDelete(r);
    SRgnCreate(&r,0);SRgnCombineRectf(r,&a,nullptr,SRGN_OR);
    SRgnCombineRectf(r,&a,nullptr,SRGN_XOR);bounds(r,0,0,0,0);
    SRgnDelete(r);SRgnDestroy();
}
static int errors;
static void error(_CMDERROR* e) { CHECK(e->errorcode && e->errorstr[0]); ++errors; }
static void registerArgs() {
    const _ARGLIST args[]={{SCMD_TYPE_BOOL,1,"flag",nullptr},{SCMD_TYPE_STRING,2,"name",nullptr}};
    CHECK(SCmdRegisterArgList(args,2));
}
static int child(int argc,char** argv) {
    registerArgs(); errno=0;
    bool unsupported=argc>1 && (!argv[argc-1][0] || std::strchr(argv[argc-1],'"') || std::strlen(argv[argc-1])>255);
    int result=SCmdProcessCommandLine(nullptr,error);
    if (unsupported) { CHECK(!result && errno==ENOTSUP && errors==1); }
    else { char text[256]; CHECK(result && SCmdGetBool(1)); CHECK(SCmdGetString(2,text,sizeof(text))); CHECK(!std::strcmp(text,argv[argc-1])); }
    CHECK(SCmdDestroy());return 0;
}
static void commands(const char* executable) {
    registerArgs();char text[64];
    CHECK(SCmdProcess("program -flag -name \"two words\"",1,nullptr,error));
    CHECK(SCmdGetBool(1) && SCmdGetString(2,text,64) && !std::strcmp(text,"two words"));
    CHECK(!SCmdProcess("-unknown",0,nullptr,error) && errors==1);
    char filename[]="/tmp/storm-algorithms-XXXXXX";int fd=mkstemp(filename);CHECK(fd>=0);
    const char response[]="-flag- -name response";CHECK(write(fd,response,sizeof(response)-1)==sizeof(response)-1);CHECK(close(fd)==0);
    std::string command="@"+std::string(filename);CHECK(SCmdProcess(command.c_str(),0,nullptr,error));CHECK(!SCmdGetBool(1));
    CHECK(SCmdGetString(2,text,64) && !std::strcmp(text,"response"));CHECK(unlink(filename)==0);
    CHECK(!SCmdProcess(command.c_str(),0,nullptr,error) && errors==2);CHECK(SCmdDestroy());
    for (const std::string& arg : {std::string("two words"),std::string("back\\slash"),std::string(""),std::string("embedded\"quote"),std::string(256,'x')}) {
        pid_t pid=fork();CHECK(pid>=0);
        if (!pid) { execl(executable,executable,"-flag","-name",arg.c_str(),nullptr);_exit(127); }
        int status;CHECK(waitpid(pid,&status,0)==pid);CHECK(WIFEXITED(status) && WEXITSTATUS(status)==0);
    }
}
int main(int argc,char** argv) {
    if (argc>1) return child(argc,argv);
    arc4();hashes();big();unicode();signatures();regions();commands(argv[0]);
    std::puts("StormAlgorithms: all real-source checks passed");
}
