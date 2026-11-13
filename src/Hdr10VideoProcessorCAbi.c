#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define CINTERFACE
#include <windows.h>
#include <d3d11_4.h>

void* P7Hdr10GetStreamSetTarget(void* videoContext) {
    ID3D11VideoContext2* context = (ID3D11VideoContext2*)videoContext;
    if (!context || !context->lpVtbl) return NULL;
    return (void*)context->lpVtbl->VideoProcessorSetStreamHDRMetaData;
}
