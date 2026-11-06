#include "jni_args.hpp"
#include <cstdio>

namespace {
using namespace rea_jni_args;
int checks = 0, failures = 0;
std::string report;
void Check(bool result, const char* reason)
{
    ++checks;
    if (!result) { ++failures; report += std::string("FAIL ") + reason + "\n"; }
}
bool Capture(Arguments* arguments, const char* signature, ...)
{
    va_list list; va_start(list, signature);
    std::string error;
    bool ok = DecodeV(signature, &list, *arguments, error);
    va_end(list); return ok;
}
void CheckCursor(void* expected, const char* signature, ...)
{
    va_list list; va_start(list, signature);
    Arguments first, second; std::string error;
    bool one = DecodeV(signature, &list, first, error);
    bool two = DecodeV(signature, &list, second, error);
    void* original = va_arg(list, void*);
    int originalDefault = va_arg(list, int);
    va_end(list);
    Check(one && two && first.size() == 2 && second.size() == 2 &&
          first[0].value.l == expected && first[1].value.i == 37 &&
          second[0].value.l == expected && second[1].value.i == 37 &&
          original == expected && originalDefault == 37, "va_copy leaves original GP cursor unchanged");
}
void CheckFpCursor(const char* signature, ...)
{
    va_list list; va_start(list, signature);
    Arguments first, second; std::string error;
    bool one = DecodeV(signature, &list, first, error);
    bool two = DecodeV(signature, &list, second, error);
    double originalFloat = va_arg(list, double), originalDouble = va_arg(list, double);
    va_end(list);
    Check(one && two && first.size() == 2 && second.size() == 2 &&
          first[0].value.f == 1.25f && first[1].value.d == -2.5 &&
          second[0].value.f == 1.25f && second[1].value.d == -2.5 &&
          originalFloat == 1.25 && originalDouble == -2.5, "va_copy leaves original FP cursor unchanged");
}
}

extern "C" const char* ReaJniArgsSelfTest()
{
    using namespace rea_jni_args;
    checks = failures = 0; report.clear();
    Arguments args; std::string error; Descriptor descriptor;
    void* reference = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x12345678));
    Check(DecodeV("()V", nullptr, args, error) && args.empty(), "zero argument V never reads source");
    Check(DecodeA("()Ljava/lang/Object;", nullptr, args, error) && args.empty(), "zero argument A never reads source");
    Check(Capture(&args,"()V") && args.empty(), "actual variadic zero argument call");
    Check(Capture(&args,"(Ljava/lang/String;Z)Z",reference,true) && args.size()==2 &&
          args[0].value.l==reference && args[1].type=='Z' && args[1].value.z==1, "getBoolean string,true");
    Check(Capture(&args,"(Ljava/lang/String;I)I",reference,37) && args.size()==2 && args[1].value.i==37, "getInt string,37");
    const JLong big = -9223372036854775000LL;
    Check(Capture(&args,"(Ljava/lang/String;J)J",reference,big) && args.size()==2 && args[1].value.j==big, "getLong exact signed 64-bit default");
    Check(Capture(&args,"(Ljava/lang/String;F)F",reference,1.25f) && args.size()==2 && args[1].value.f==1.25f, "getFloat C double promotion");
    Check(Capture(&args,"(Ljava/lang/String;D)D",reference,-2.5) && args.size()==2 && args[1].value.d==-2.5, "double default");
    Check(Capture(&args,"(ZBCSI)V",true,-5,65530,-1234,-98765) && args.size()==5 &&
          args[0].value.z==1 && args[1].value.b==-5 && args[2].value.c==65530 &&
          args[3].value.s==-1234 && args[4].value.i==-98765, "small integral default promotions");
    Check(Capture(&args,"([I[[Ljava/lang/String;Ljava/lang/Object;I)V",reference,static_cast<void*>(nullptr),reference,37) && args.size()==4 &&
          args[0].type=='L' && args[1].type=='L' && args[0].value.l==reference && !args[1].value.l &&
          args[2].value.l==reference && args[3].value.i==37, "arrays/objects consume one reference each");
    Check(Capture(&args,"(Ljava/lang/String;IZZZZLjava/lang/String;IZZ)V",reference,4,true,false,true,false,reference,23,true,false)&&args.size()==10&&
          args[0].value.l==reference&&args[4].value.z==1&&args[6].value.l==reference&&args[7].value.i==23&&args[8].value.z==1&&!args[9].value.z,
          "Unity keyboard ten mixed reference/bool/int parameters including stack");
    CheckCursor(reference,"(Ljava/lang/String;I)I",reference,37);
    CheckFpCursor("(FD)V",1.25f,-2.5);
    bool gp = Capture(&args,"(IIIIIIIIII)V",1,2,3,4,5,6,7,8,9,10) && args.size()==10;
    if (gp) for (int index=0; index<10; ++index) gp = gp && args[index].value.i==index+1;
    Check(gp,"AArch64 GP arguments spill to stack");
    bool fp = Capture(&args,"(DDDDDDDDDD)V",1.0,2.0,3.0,4.0,5.0,6.0,7.0,8.0,9.0,10.0) && args.size()==10;
    if (fp) for (int index=0; index<10; ++index) fp = fp && args[index].value.d==index+1;
    Check(fp,"AArch64 FP arguments spill to stack");
    JValue values[8]{};
    values[0].l=reference; values[1].z=1; values[2].i=37; values[3].j=big;
    values[4].f=1.25f; values[5].d=-2.5; values[6].l=nullptr; values[7].s=-1234;
    Check(DecodeA("(Ljava/lang/String;ZIJFD[[IS)V",values,args,error) && args.size()==8 &&
          args[0].value.l==reference && args[1].value.z==1 && args[2].value.i==37 && args[3].value.j==big &&
          args[4].value.f==1.25f && args[5].value.d==-2.5 && !args[6].value.l && args[7].value.s==-1234,
          "A typed union values, unpromoted float and null array");
    Check(!DecodeV("(Ljava/lang/String;)V",nullptr,args,error) && !error.empty(), "nonempty V null source rejected");
    Check(!DecodeA("(I)V",nullptr,args,error) && !error.empty(), "nonempty A null source rejected");
    for (const char* bad : {"", "I)V", "(V)V", "([V)V", "(L;)V", "(Ljava/lang/String)V", "(I)", "(I)Vjunk", "(I)V;", "(Q)V", "(II"}) {
        Check(!DecodeV(bad,nullptr,args,error) && !error.empty(), "malformed descriptor rejected before reading source");
    }
    Check(Parse("(Ljava/lang/String;[[IJFD)[[Ljava/lang/Object;",descriptor,error) &&
          descriptor.parameters.size()==5 && descriptor.parameters[1]=='L' && descriptor.result=='L', "full nested object/array descriptor");
    report += "JNI_ARGS_SELFTEST "; report += failures ? "FAIL" : "PASS";
    report += " checks="+std::to_string(checks)+" failed="+std::to_string(failures)+"\n";
    return report.c_str();
}

#ifdef REA_JNI_ARGS_TEST_MAIN
int main() { const char* value=ReaJniArgsSelfTest(); std::fputs(value,stdout); return failures ? 1 : 0; }
#endif
