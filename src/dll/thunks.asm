; First-call thunks for the D3DCompiler_43 proxy.
; Four arg registers are saved, LoadGenuineDll runs, then we jump to the real export.
; Four pushes leave RSP 8 mod 16. sub rsp, 28h is 32 bytes of shadow space
; plus 8 bytes so RSP is 0 mod 16 before the call. The original 20h value
; entered LoadGenuineDll misaligned and ntdll faulted on movaps.

EXTERN LoadGenuineDll:PROC
EXTERN D3DAssemble_:QWORD
EXTERN DebugSetMute_:QWORD
EXTERN D3DCompile_:QWORD
EXTERN D3DCompressShaders_:QWORD
EXTERN D3DCreateBlob_:QWORD
EXTERN D3DDecompressShaders_:QWORD
EXTERN D3DDisassemble_:QWORD
EXTERN D3DDisassemble10Effect_:QWORD
EXTERN D3DGetBlobPart_:QWORD
EXTERN D3DGetDebugInfo_:QWORD
EXTERN D3DGetInputAndOutputSignatureBlob_:QWORD
EXTERN D3DGetInputSignatureBlob_:QWORD
EXTERN D3DGetOutputSignatureBlob_:QWORD
EXTERN D3DPreprocess_:QWORD
EXTERN D3DReflect_:QWORD
EXTERN D3DReturnFailure1_:QWORD
EXTERN D3DStripShader_:QWORD

.code

MAKE_THUNK MACRO name
name PROC
    push r9
    push r8
    push rdx
    push rcx
    sub rsp, 28h
    call LoadGenuineDll
    add rsp, 28h
    pop rcx
    pop rdx
    pop r8
    pop r9
    jmp QWORD PTR [name&_]
name ENDP
ENDM

MAKE_THUNK D3DAssemble
MAKE_THUNK DebugSetMute
MAKE_THUNK D3DCompile
MAKE_THUNK D3DCompressShaders
MAKE_THUNK D3DCreateBlob
MAKE_THUNK D3DDecompressShaders
MAKE_THUNK D3DDisassemble
MAKE_THUNK D3DDisassemble10Effect
MAKE_THUNK D3DGetBlobPart
MAKE_THUNK D3DGetDebugInfo
MAKE_THUNK D3DGetInputAndOutputSignatureBlob
MAKE_THUNK D3DGetInputSignatureBlob
MAKE_THUNK D3DGetOutputSignatureBlob
MAKE_THUNK D3DPreprocess
MAKE_THUNK D3DReflect
MAKE_THUNK D3DReturnFailure1
MAKE_THUNK D3DStripShader

END
