#ifndef UEFI_H
#define UEFI_H

/*
 * The slice of UEFI that cix-boot needs, declared here rather than taken
 * from gnu-efi (ADR-0215).
 *
 * Not because gnu-efi is unavailable -- it is a package here already,
 * built for shim -- but because its value is its build machinery (crt0,
 * relocation handling, and an ELF-then-objcopy conversion whose
 * `efi-app-x86_64` target modern binutils no longer provides), and
 * linking PE directly -- once with `ld -m i386pep`, now with tcc's own
 * PE linker (ADR-0362) -- needs none of it. What is
 * left to take is structure definitions fixed by a published
 * specification, which is what this file is.
 *
 * Same posture as include/linux_compat.h: declare the handful of things
 * we actually use, correctly, rather than depend on a header that
 * carries a build system with it.
 *
 * TWO RULES govern everything below, and getting either wrong produces a
 * binary that faults inside firmware with no diagnostic:
 *
 *   1. Every protocol function is EFIAPI. UEFI uses the Microsoft x64
 *      calling convention, not the System V one a Linux compiler
 *      defaults to. This file is compiled by tcc's x86_64-win32 cross
 *      target (the tcc-win64 package, ADR-0362), whose DEFAULT
 *      convention is the Microsoft one, so EFIAPI is empty and the
 *      #error below refuses any compiler that is not that target. tcc
 *      has no ms_abi attribute (measured, probe-tccefi@5-1), so the
 *      attribute could not have done the work under it anyway.
 *
 *   2. Every structure's member ORDER and SIZE must match the spec
 *      exactly, including members we never call. Firmware hands us a
 *      pointer to its own table; a missing or reordered field silently
 *      shifts every later one. Unused members are therefore declared as
 *      void * placeholders and named, never omitted.
 */

#ifndef _WIN64
#error "cix-boot needs a compiler whose default calling convention is the Microsoft x64 one: tcc's x86_64-win32 target (EFI_CC, ADR-0362)"
#endif
#define EFIAPI

/*
 * A UEFI wide string literal. On the win64 target L"..." is 16-bit,
 * which is the right width, but its type is wchar_t while the spec's
 * CHAR16 is unsigned short -- so every literal needs a cast, and a macro
 * is better than scattering casts through the code. The assert keeps a
 * compiler with 32-bit wide characters from producing UCS-4 strings the
 * firmware would read as every other character.
 */
_Static_assert(sizeof(L"x"[0]) == 2, "UEFI strings are 16-bit: L\"\" must be too");
#define L16(s) ((CHAR16 *)L##s)

typedef unsigned short CHAR16;
typedef unsigned char UINT8;
typedef unsigned short UINT16;
typedef unsigned int UINT32;
typedef unsigned long long UINT64;
typedef long long INT64;
/*
 * UINTN is the native width, 64 bits on x86-64, and NOT `unsigned long`:
 * the win64 target is LLP64, where long is 32 bits. It was `unsigned
 * long` while gcc built this for an LP64 target, and under the win64
 * compiler every UINTN field would have shrunk and shifted every UEFI
 * table after it (ADR-0362). The asserts make either mistake a compile
 * error rather than a fault inside firmware.
 */
typedef unsigned long long UINTN;
_Static_assert(sizeof(UINTN) == 8, "UINTN is the native width, 64 bits on x86-64");
_Static_assert(sizeof(void *) == 8, "cix-boot is an x86-64 EFI application");
typedef unsigned char BOOLEAN;
typedef void *EFI_HANDLE;
typedef void *EFI_EVENT;
typedef UINTN EFI_STATUS;

#define EFI_SUCCESS 0
/* The high bit of a UINTN marks an error; only the codes we act on. */
#define EFI_ERR(n) ((EFI_STATUS)((UINTN)1 << (sizeof(UINTN) * 8 - 1)) | (n))
#define EFI_LOAD_ERROR EFI_ERR(1)
#define EFI_INVALID_PARAMETER EFI_ERR(2)
#define EFI_UNSUPPORTED EFI_ERR(3)
#define EFI_BUFFER_TOO_SMALL EFI_ERR(5)
#define EFI_NOT_FOUND EFI_ERR(14)
#define EFI_ERROR(s) (((INT64)(s)) < 0)

typedef struct {
	UINT32 Data1;
	UINT16 Data2;
	UINT16 Data3;
	UINT8 Data4[8];
} EFI_GUID;

/* File open modes and attributes (UEFI spec, EFI_FILE_PROTOCOL.Open). */
#define EFI_FILE_MODE_READ 0x0000000000000001ULL
#define EFI_FILE_MODE_WRITE 0x0000000000000002ULL
#define EFI_FILE_DIRECTORY 0x0000000000000010ULL

typedef struct {
	UINT16 Year;
	UINT8 Month;
	UINT8 Day;
	UINT8 Hour;
	UINT8 Minute;
	UINT8 Second;
	UINT8 Pad1;
	UINT32 Nanosecond;
	UINT16 TimeZone;
	UINT8 Daylight;
	UINT8 Pad2;
} EFI_TIME;

/*
 * EFI_FILE_INFO. The trailing FileName is a variable-length CHAR16
 * array, so this struct is always used inside a caller-provided buffer
 * larger than sizeof(EFI_FILE_INFO).
 */
typedef struct {
	UINT64 Size;
	UINT64 FileSize;
	UINT64 PhysicalSize;
	EFI_TIME CreateTime;
	EFI_TIME LastAccessTime;
	EFI_TIME ModificationTime;
	UINT64 Attribute;
	CHAR16 FileName[1];
} EFI_FILE_INFO;

#define EFI_FILE_INFO_GUID                                                                         \
	{                                                                                          \
		0x09576e92, 0x6d3f, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b }     \
	}

struct _EFI_FILE_PROTOCOL;
typedef struct _EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;

struct _EFI_FILE_PROTOCOL {
	UINT64 Revision;
	EFI_STATUS(EFIAPI *Open)
	(EFI_FILE_PROTOCOL *This, EFI_FILE_PROTOCOL **NewHandle, CHAR16 *FileName,
	 UINT64 OpenMode, UINT64 Attributes);
	EFI_STATUS(EFIAPI *Close)(EFI_FILE_PROTOCOL *This);
	EFI_STATUS(EFIAPI *Delete)(EFI_FILE_PROTOCOL *This);
	EFI_STATUS(EFIAPI *Read)(EFI_FILE_PROTOCOL *This, UINTN *BufferSize, void *Buffer);
	EFI_STATUS(EFIAPI *Write)(EFI_FILE_PROTOCOL *This, UINTN *BufferSize, void *Buffer);
	EFI_STATUS(EFIAPI *GetPosition)(EFI_FILE_PROTOCOL *This, UINT64 *Position);
	EFI_STATUS(EFIAPI *SetPosition)(EFI_FILE_PROTOCOL *This, UINT64 Position);
	EFI_STATUS(EFIAPI *GetInfo)
	(EFI_FILE_PROTOCOL *This, EFI_GUID *InformationType, UINTN *BufferSize, void *Buffer);
	EFI_STATUS(EFIAPI *SetInfo)
	(EFI_FILE_PROTOCOL *This, EFI_GUID *InformationType, UINTN BufferSize, void *Buffer);
	EFI_STATUS(EFIAPI *Flush)(EFI_FILE_PROTOCOL *This);
};

typedef struct {
	UINT64 Revision;
	EFI_STATUS(EFIAPI *OpenVolume)(void *This, EFI_FILE_PROTOCOL **Root);
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID                                                       \
	{                                                                                          \
		0x0964e5b22, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b }    \
	}

/*
 * EFI_LOADED_IMAGE_PROTOCOL. DeviceHandle is what cix-boot is really
 * after: the handle of the volume it was itself loaded from, i.e. the
 * ESP, so entries are read from the same partition the boot manager
 * came from rather than from a guess.
 */
typedef struct {
	UINT32 Revision;
	EFI_HANDLE ParentHandle;
	void *SystemTable;
	EFI_HANDLE DeviceHandle;
	void *FilePath;
	void *Reserved;
	UINT32 LoadOptionsSize;
	void *LoadOptions;
	void *ImageBase;
	UINT64 ImageSize;
	UINT32 ImageCodeType;
	UINT32 ImageDataType;
	void *Unload;
} EFI_LOADED_IMAGE_PROTOCOL;

#define EFI_LOADED_IMAGE_PROTOCOL_GUID                                                             \
	{                                                                                          \
		0x5b1b31a1, 0x9562, 0x11d2, { 0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b }     \
	}

typedef struct {
	void *Reset;
	EFI_STATUS(EFIAPI *OutputString)(void *This, CHAR16 *String);
	void *TestString;
	void *QueryMode;
	void *SetMode;
	void *SetAttribute;
	EFI_STATUS(EFIAPI *ClearScreen)(void *This);
	void *SetCursorPosition;
	void *EnableCursor;
	void *Mode;
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

typedef struct {
	UINT64 Signature;
	UINT32 Revision;
	UINT32 HeaderSize;
	UINT32 CRC32;
	UINT32 Reserved;
} EFI_TABLE_HEADER;

/*
 * EFI_BOOT_SERVICES. Every member is present and in spec order --
 * see rule 2 at the top of this file. The ones cix-boot calls are typed;
 * the rest are void * placeholders carrying their real names, so the
 * offsets are right and a future caller can type one without having to
 * re-derive the layout.
 */
typedef struct {
	EFI_TABLE_HEADER Hdr;

	void *RaiseTPL;
	void *RestoreTPL;

	void *AllocatePages;
	void *FreePages;
	void *GetMemoryMap;
	EFI_STATUS(EFIAPI *AllocatePool)(UINT32 PoolType, UINTN Size, void **Buffer);
	EFI_STATUS(EFIAPI *FreePool)(void *Buffer);

	void *CreateEvent;
	void *SetTimer;
	void *WaitForEvent;
	void *SignalEvent;
	void *CloseEvent;
	void *CheckEvent;

	void *InstallProtocolInterface;
	void *ReinstallProtocolInterface;
	void *UninstallProtocolInterface;
	EFI_STATUS(EFIAPI *HandleProtocol)(EFI_HANDLE Handle, EFI_GUID *Protocol, void **Interface);
	void *Reserved;
	void *RegisterProtocolNotify;
	void *LocateHandle;
	void *LocateDevicePath;
	void *InstallConfigurationTable;

	EFI_STATUS(EFIAPI *LoadImage)
	(BOOLEAN BootPolicy, EFI_HANDLE ParentImageHandle, void *DevicePath, void *SourceBuffer,
	 UINTN SourceSize, EFI_HANDLE *ImageHandle);
	EFI_STATUS(EFIAPI *StartImage)(EFI_HANDLE ImageHandle, UINTN *ExitDataSize, CHAR16 **ExitData);
	EFI_STATUS(EFIAPI *Exit)
	(EFI_HANDLE ImageHandle, EFI_STATUS ExitStatus, UINTN ExitDataSize, CHAR16 *ExitData);
	void *UnloadImage;
	void *ExitBootServices;

	void *GetNextMonotonicCount;
	EFI_STATUS(EFIAPI *Stall)(UINTN Microseconds);
	void *SetWatchdogTimer;

	void *ConnectController;
	void *DisconnectController;

	void *OpenProtocol;
	void *CloseProtocol;
	void *OpenProtocolInformation;

	void *ProtocolsPerHandle;
	void *LocateHandleBuffer;
	void *LocateProtocol;
	void *InstallMultipleProtocolInterfaces;
	void *UninstallMultipleProtocolInterfaces;

	void *CalculateCrc32;

	void *CopyMem;
	void *SetMem;
	void *CreateEventEx;
} EFI_BOOT_SERVICES;

/*
 * EFI_RUNTIME_SERVICES. Same posture as EFI_BOOT_SERVICES above: every
 * member present in spec order, typed only where cix-boot actually
 * calls it (GetVariable/SetVariable, for the boot-loader-interface
 * LoaderEntryOneShot variable -- #469), the rest void * placeholders
 * carrying their real names so the offsets stay right.
 */
typedef struct {
	EFI_TABLE_HEADER Hdr;

	void *GetTime;
	void *SetTime;
	void *GetWakeupTime;
	void *SetWakeupTime;

	void *SetVirtualAddressMap;
	void *ConvertPointer;

	EFI_STATUS(EFIAPI *GetVariable)
	(CHAR16 *VariableName, EFI_GUID *VendorGuid, UINT32 *Attributes, UINTN *DataSize,
	 void *Data);
	void *GetNextVariableName;
	EFI_STATUS(EFIAPI *SetVariable)
	(CHAR16 *VariableName, EFI_GUID *VendorGuid, UINT32 Attributes, UINTN DataSize,
	 void *Data);

	void *GetNextHighMonotonicCount;
	void *ResetSystem;

	void *UpdateCapsule;
	void *QueryCapsuleCapabilities;
	void *QueryVariableInfo;
} EFI_RUNTIME_SERVICES;

typedef struct {
	EFI_TABLE_HEADER Hdr;
	CHAR16 *FirmwareVendor;
	UINT32 FirmwareRevision;
	EFI_HANDLE ConsoleInHandle;
	void *ConIn;
	EFI_HANDLE ConsoleOutHandle;
	EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
	EFI_HANDLE StandardErrorHandle;
	EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
	EFI_RUNTIME_SERVICES *RuntimeServices;
	EFI_BOOT_SERVICES *BootServices;
	UINTN NumberOfTableEntries;
	void *ConfigurationTable;
} EFI_SYSTEM_TABLE;

/* EFI_LOADER_DATA, for AllocatePool. */
#define EfiLoaderData 2

#endif /* UEFI_H */
