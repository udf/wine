/*
 * DOS (MZ) loader
 *
 * Copyright 1998 Ove K�ven
 *
 * This code hasn't been completely cleaned up yet.
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include "windef.h"
#include "wine/winbase16.h"
#include "winerror.h"
#include "module.h"
#include "task.h"
#include "file.h"
#include "miscemu.h"
#include "debugtools.h"
#include "dosexe.h"
#include "dosmod.h"
#include "options.h"
#include "vga.h"

DEFAULT_DEBUG_CHANNEL(module);

static LPDOSTASK dos_current;

#ifdef MZ_SUPPORTED

#ifdef HAVE_SYS_MMAN_H
# include <sys/mman.h>
#endif

/* define this to try mapping through /proc/pid/mem instead of a temp file,
   but Linus doesn't like mmapping /proc/pid/mem, so it doesn't work for me */
#undef MZ_MAPSELF

#define BIOS_DATA_SEGMENT 0x40
#define PSP_SIZE 0x10

#define SEG16(ptr,seg) ((LPVOID)((BYTE*)ptr+((DWORD)(seg)<<4)))
#define SEGPTR16(ptr,segptr) ((LPVOID)((BYTE*)ptr+((DWORD)SELECTOROF(segptr)<<4)+OFFSETOF(segptr)))

/* structures for EXEC */

typedef struct {
  WORD env_seg;
  DWORD cmdline WINE_PACKED;
  DWORD fcb1 WINE_PACKED;
  DWORD fcb2 WINE_PACKED;
  WORD init_sp;
  WORD init_ss;
  WORD init_ip;
  WORD init_cs;
} ExecBlock;

typedef struct {
  WORD load_seg;
  WORD rel_seg;
} OverlayBlock;

/* global variables */

static WORD init_cs,init_ip,init_ss,init_sp;
static char mm_name[128];

int read_pipe, write_pipe;
HANDLE hReadPipe, hWritePipe;
pid_t dosmod_pid;

static void MZ_Launch(void);
static BOOL MZ_InitTask(void);
static void MZ_KillTask(void);

static void MZ_CreatePSP( LPVOID lpPSP, WORD env, WORD par )
{
  PDB16*psp=lpPSP;

  psp->int20=0x20CD; /* int 20 */
  /* some programs use this to calculate how much memory they need */
  psp->nextParagraph=0x9FFF; /* FIXME: use a real value */
  /* FIXME: dispatcher */
  psp->savedint22 = INT_GetRMHandler(0x22);
  psp->savedint23 = INT_GetRMHandler(0x23);
  psp->savedint24 = INT_GetRMHandler(0x24);
  psp->parentPSP=par;
  psp->environment=env;
  /* FIXME: more PSP stuff */
}

static void MZ_FillPSP( LPVOID lpPSP, LPCSTR cmdline )
{
 PDB16*psp=lpPSP;
 const char*cmd=cmdline?strchr(cmdline,' '):NULL;

 /* copy parameters */
 if (cmd) {
#if 0
  /* command.com doesn't do this */
  while (*cmd == ' ') cmd++;
#endif
  psp->cmdLine[0]=strlen(cmd);
  strcpy(psp->cmdLine+1,cmd);
  psp->cmdLine[psp->cmdLine[0]+1]='\r';
 } else psp->cmdLine[1]='\r';
 /* FIXME: more PSP stuff */
}

/* default INT 08 handler: increases timer tick counter but not much more */
static char int08[]={
 0xCD,0x1C,           /* int $0x1c */
 0x50,                /* pushw %ax */
 0x1E,                /* pushw %ds */
 0xB8,0x40,0x00,      /* movw $0x40,%ax */
 0x8E,0xD8,           /* movw %ax,%ds */
#if 0
 0x83,0x06,0x6C,0x00,0x01, /* addw $1,(0x6c) */
 0x83,0x16,0x6E,0x00,0x00, /* adcw $0,(0x6e) */
#else
 0x66,0xFF,0x06,0x6C,0x00, /* incl (0x6c) */
#endif
 0xB0,0x20,           /* movb $0x20,%al */
 0xE6,0x20,           /* outb %al,$0x20 */
 0x1F,                /* popw %ax */
 0x58,                /* popw %ax */
 0xCF                 /* iret */
};

static void MZ_InitHandlers(void)
{
 WORD seg;
 LPBYTE start=DOSMEM_GetBlock(sizeof(int08),&seg);
 memcpy(start,int08,sizeof(int08));
/* INT 08: point it at our tick-incrementing handler */
 ((SEGPTR*)0)[0x08]=MAKESEGPTR(seg,0);
/* INT 1C: just point it to IRET, we don't want to handle it ourselves */
 ((SEGPTR*)0)[0x1C]=MAKESEGPTR(seg,sizeof(int08)-1);
}

static WORD MZ_InitEnvironment( LPCSTR env, LPCSTR name )
{
 unsigned sz=0;
 WORD seg;
 LPSTR envblk;

 if (env) {
  /* get size of environment block */
  while (env[sz++]) sz+=strlen(env+sz)+1;
 } else sz++;
 /* allocate it */
 envblk=DOSMEM_GetBlock(sz+sizeof(WORD)+strlen(name)+1,&seg);
 /* fill it */
 if (env) {
  memcpy(envblk,env,sz);
 } else envblk[0]=0;
 /* DOS 3.x: the block contains 1 additional string */
 *(WORD*)(envblk+sz)=1;
 /* being the program name itself */
 strcpy(envblk+sz+sizeof(WORD),name);
 return seg;
}

static BOOL MZ_InitMemory(void)
{
    int mm_fd;
    void *img_base;

    /* initialize the memory */
    TRACE("Initializing DOS memory structures\n");
    DOSMEM_Init(TRUE);

    /* allocate 1MB+64K shared memory */
    tmpnam(mm_name);
    /* strcpy(mm_name,"/tmp/mydosimage"); */
    mm_fd = open(mm_name,O_RDWR|O_CREAT /* |O_TRUNC */,S_IRUSR|S_IWUSR);
    if (mm_fd < 0) ERR("file %s could not be opened\n",mm_name);
    /* fill the file with the DOS memory */
    if (write( mm_fd, NULL, 0x110000 ) != 0x110000) ERR("cannot write DOS mem\n");
    /* map it in */
    img_base = mmap(NULL,0x110000,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_SHARED|MAP_FIXED,mm_fd,0);
    close( mm_fd );

    if (img_base)
    {
        ERR("could not map shared memory, error=%s\n",strerror(errno));
        return FALSE;
    }
    MZ_InitHandlers();
    return TRUE;
}

BOOL MZ_DoLoadImage( HANDLE hFile, LPCSTR filename, OverlayBlock *oblk )
{
  LPDOSTASK lpDosTask = dos_current;
  IMAGE_DOS_HEADER mz_header;
  DWORD image_start,image_size,min_size,max_size,avail;
  BYTE*psp_start,*load_start,*oldenv;
  int x, old_com=0, alloc;
  SEGPTR reloc;
  WORD env_seg, load_seg, rel_seg, oldpsp_seg;
  DWORD len;

  if (lpDosTask) {
    /* DOS process already running, inherit from it */
    PDB16* par_psp = (PDB16*)((DWORD)lpDosTask->psp_seg << 4);
    alloc=0;
    oldenv = (LPBYTE)((DWORD)par_psp->environment << 4);
    oldpsp_seg = lpDosTask->psp_seg;
  } else {
    /* allocate new DOS process, inheriting from Wine environment */
    alloc=1;
    lpDosTask = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DOSTASK));
    dos_current = lpDosTask;
    oldenv = GetEnvironmentStringsA();
    oldpsp_seg = 0;
  }

 SetFilePointer(hFile,0,NULL,FILE_BEGIN);
 if (   !ReadFile(hFile,&mz_header,sizeof(mz_header),&len,NULL)
     || len != sizeof(mz_header) 
     || mz_header.e_magic != IMAGE_DOS_SIGNATURE) {
  char *p = strrchr( filename, '.' );
  if (!p || strcasecmp( p, ".com" ))  /* check for .COM extension */
  {
      SetLastError(ERROR_BAD_FORMAT);
      goto load_error;
  }
  old_com=1; /* assume .COM file */
  image_start=0;
  image_size=GetFileSize(hFile,NULL);
  min_size=0x10000; max_size=0x100000;
  mz_header.e_crlc=0;
  mz_header.e_ss=0; mz_header.e_sp=0xFFFE;
  mz_header.e_cs=0; mz_header.e_ip=0x100;
 } else {
  /* calculate load size */
  image_start=mz_header.e_cparhdr<<4;
  image_size=mz_header.e_cp<<9; /* pages are 512 bytes */
  if ((mz_header.e_cblp!=0)&&(mz_header.e_cblp!=4)) image_size-=512-mz_header.e_cblp;
  image_size-=image_start;
  min_size=image_size+((DWORD)mz_header.e_minalloc<<4)+(PSP_SIZE<<4);
  max_size=image_size+((DWORD)mz_header.e_maxalloc<<4)+(PSP_SIZE<<4);
 }

  if (alloc) MZ_InitMemory();

  if (oblk) {
    /* load overlay into preallocated memory */
    load_seg=oblk->load_seg;
    rel_seg=oblk->rel_seg;
    load_start=(LPBYTE)((DWORD)load_seg<<4);
  } else {
    /* allocate environment block */
    env_seg=MZ_InitEnvironment(oldenv, filename);

    /* allocate memory for the executable */
    TRACE("Allocating DOS memory (min=%ld, max=%ld)\n",min_size,max_size);
    avail=DOSMEM_Available();
    if (avail<min_size) {
      ERR("insufficient DOS memory\n");
      SetLastError(ERROR_NOT_ENOUGH_MEMORY);
      goto load_error;
    }
    if (avail>max_size) avail=max_size;
    psp_start=DOSMEM_GetBlock(avail,&lpDosTask->psp_seg);
    if (!psp_start) {
      ERR("error allocating DOS memory\n");
      SetLastError(ERROR_NOT_ENOUGH_MEMORY);
      goto load_error;
    }
    load_seg=lpDosTask->psp_seg+(old_com?0:PSP_SIZE);
    rel_seg=load_seg;
    load_start=psp_start+(PSP_SIZE<<4);
    MZ_CreatePSP(psp_start, env_seg, oldpsp_seg);
  }

 /* load executable image */
 TRACE("loading DOS %s image, %08lx bytes\n",old_com?"COM":"EXE",image_size);
 SetFilePointer(hFile,image_start,NULL,FILE_BEGIN);
 if (!ReadFile(hFile,load_start,image_size,&len,NULL) || len != image_size) {
  SetLastError(ERROR_BAD_FORMAT);
  goto load_error;
 }

 if (mz_header.e_crlc) {
  /* load relocation table */
  TRACE("loading DOS EXE relocation table, %d entries\n",mz_header.e_crlc);
  /* FIXME: is this too slow without read buffering? */
  SetFilePointer(hFile,mz_header.e_lfarlc,NULL,FILE_BEGIN);
  for (x=0; x<mz_header.e_crlc; x++) {
   if (!ReadFile(hFile,&reloc,sizeof(reloc),&len,NULL) || len != sizeof(reloc)) {
    SetLastError(ERROR_BAD_FORMAT);
    goto load_error;
   }
   *(WORD*)SEGPTR16(load_start,reloc)+=rel_seg;
  }
 }

  if (!oblk) {
    init_cs = load_seg+mz_header.e_cs;
    init_ip = mz_header.e_ip;
    init_ss = load_seg+mz_header.e_ss;
    init_sp = mz_header.e_sp;

    TRACE("entry point: %04x:%04x\n",init_cs,init_ip);
  }

  if (alloc && !MZ_InitTask()) {
    MZ_KillTask();
    SetLastError(ERROR_GEN_FAILURE);
    return FALSE;
  }

  return TRUE;

load_error:
  lpDosTask->psp_seg = oldpsp_seg;
  if (alloc) {
    dos_current = NULL;
    if (mm_name[0]!=0) unlink(mm_name);
  }

  return FALSE;
}

BOOL MZ_LoadImage( LPCSTR cmdline )
{
    HFILE hFile;
    char *name, buffer[MAX_PATH];
    LPCSTR p = strchr( cmdline, ' ' );

    if (p)
    {
        if (!(name = HeapAlloc( GetProcessHeap(), 0, p - cmdline + 1 ))) return FALSE;
        memcpy( name, cmdline, p - cmdline );
        name[p - cmdline] = 0;
    }
    else name = (char *)cmdline;

    if (!SearchPathA( NULL, name, ".exe", sizeof(buffer), buffer, NULL )) goto error;
    if ((hFile = CreateFileA( buffer, GENERIC_READ, FILE_SHARE_READ,
                              NULL, OPEN_EXISTING, 0, -1 )) == INVALID_HANDLE_VALUE)
        goto error;
    if (!MZ_DoLoadImage( hFile, buffer, NULL ))
    {
        CloseHandle( hFile );
        goto error;
    }
    MZ_Launch();
 error:
    if (name != cmdline) HeapFree( GetProcessHeap(), 0, name );
    return FALSE;
}

BOOL MZ_Exec( CONTEXT86 *context, LPCSTR filename, BYTE func, LPVOID paramblk )
{
  /* this may only be called from existing DOS processes
   * (i.e. one DOS app spawning another) */
  /* FIXME: do we want to check binary type first, to check
   * whether it's a NE/PE executable? */
  LPDOSTASK lpDosTask = MZ_Current();
  HFILE hFile = CreateFileA( filename, GENERIC_READ, FILE_SHARE_READ,
			     NULL, OPEN_EXISTING, 0, -1);
  BOOL ret = FALSE;
  if (hFile == INVALID_HANDLE_VALUE) return FALSE;
  switch (func) {
  case 0: /* load and execute */
  case 1: /* load but don't execute */
    {
      /* save current process's return SS:SP now */
      LPBYTE psp_start = (LPBYTE)((DWORD)lpDosTask->psp_seg << 4);
      PDB16 *psp = (PDB16 *)psp_start;
      psp->saveStack = (DWORD)MAKESEGPTR(context->SegSs, LOWORD(context->Esp));
    }
    ret = MZ_DoLoadImage( hFile, filename, NULL );
    if (ret) {
      /* MZ_LoadImage created a new PSP and loaded new values into lpDosTask,
       * let's work on the new values now */
      LPBYTE psp_start = (LPBYTE)((DWORD)lpDosTask->psp_seg << 4);
      ExecBlock *blk = (ExecBlock *)paramblk;
      MZ_FillPSP(psp_start, DOSMEM_MapRealToLinear(blk->cmdline));
      /* the lame MS-DOS engineers decided that the return address should be in int22 */
      INT_SetRMHandler(0x22, (FARPROC16)MAKESEGPTR(context->SegCs, LOWORD(context->Eip)));
      if (func) {
	/* don't execute, just return startup state */
	blk->init_cs = init_cs;
	blk->init_ip = init_ip;
	blk->init_ss = init_ss;
	blk->init_sp = init_sp;
      } else {
	/* execute by making us return to new process */
	context->SegCs = init_cs;
	context->Eip   = init_ip;
	context->SegSs = init_ss;
	context->Esp   = init_sp;
	context->SegDs = lpDosTask->psp_seg;
	context->SegEs = lpDosTask->psp_seg;
	context->Eax   = 0;
      }
    }
    break;
  case 3: /* load overlay */
    {
      OverlayBlock *blk = (OverlayBlock *)paramblk;
      ret = MZ_DoLoadImage( hFile, filename, blk );
    }
    break;
  default:
    FIXME("EXEC load type %d not implemented\n", func);
    SetLastError(ERROR_INVALID_FUNCTION);
    break;
  }
  CloseHandle(hFile);
  return ret;
}

LPDOSTASK MZ_AllocDPMITask( void )
{
  LPDOSTASK lpDosTask = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DOSTASK));

  if (lpDosTask) {
    dos_current = lpDosTask;
    MZ_InitMemory();
    MZ_InitTask();
  }
  return lpDosTask;
}

static void MZ_InitTimer( int ver )
{
 if (ver<1) {
  /* can't make timer ticks */
 } else {
  int func;
  struct timeval tim;

  /* start dosmod timer at 55ms (18.2Hz) */
  func=DOSMOD_SET_TIMER;
  tim.tv_sec=0; tim.tv_usec=54925;
  write(write_pipe,&func,sizeof(func));
  write(write_pipe,&tim,sizeof(tim));
 }
}

static BOOL MZ_InitTask(void)
{
  int write_fd[2],x_fd;
  pid_t child;
  char path[256],*fpath;

  /* create pipes */
  if (!CreatePipe(&hReadPipe,&hWritePipe,NULL,0)) return FALSE;
  if (pipe(write_fd)<0) {
    CloseHandle(hReadPipe);
    CloseHandle(hWritePipe);
    return FALSE;
  }

  read_pipe = FILE_GetUnixHandle( hReadPipe, GENERIC_READ );
  x_fd = FILE_GetUnixHandle( hWritePipe, GENERIC_WRITE );

  TRACE("win32 pipe: read=%d, write=%d, unix pipe: read=%d, write=%d\n",
        hReadPipe,hWritePipe,read_pipe,x_fd);
  TRACE("outbound unix pipe: read=%d, write=%d, pid=%d\n",write_fd[0],write_fd[1],getpid());

  write_pipe=write_fd[1];

  TRACE("Loading DOS VM support module\n");
  if ((child=fork())<0) {
    close(write_fd[0]);
    close(read_pipe);
    close(write_pipe);
    close(x_fd);
    CloseHandle(hReadPipe);
    CloseHandle(hWritePipe);
    return FALSE;
  }
 if (child!=0) {
  /* parent process */
  int ret;

  close(write_fd[0]);
  close(x_fd);
  dosmod_pid = child;
  /* wait for child process to signal readiness */
  while (1) {
    if (read(read_pipe,&ret,sizeof(ret))==sizeof(ret)) break;
    if ((errno==EINTR)||(errno==EAGAIN)) continue;
    /* failure */
    ERR("dosmod has failed to initialize\n");
    if (mm_name[0]!=0) unlink(mm_name);
    return FALSE;
  }
  /* the child has now mmaped the temp file, it's now safe to unlink.
   * do it here to avoid leaving a mess in /tmp if/when Wine crashes... */
  if (mm_name[0]!=0) unlink(mm_name);
  /* start simulated system timer */
  MZ_InitTimer(ret);
  if (ret<2) {
    ERR("dosmod version too old! Please install newer dosmod properly\n");
    ERR("If you don't, the new dosmod event handling system will not work\n");
  }
  /* all systems are now go */
 } else {
  /* child process */
  close(read_pipe);
  close(write_pipe);
  /* put our pipes somewhere dosmod can find them */
  dup2(write_fd[0],0); /* stdin */
  dup2(x_fd,1);        /* stdout */
  /* now load dosmod */
  /* check argv[0]-derived paths first, since the newest dosmod is most likely there
   * (at least it was once for Andreas Mohr, so I decided to make it easier for him) */
  fpath=strrchr(strcpy(path,full_argv0),'/');
  if (fpath) {
   strcpy(fpath,"/dosmod");
   execl(path,mm_name,NULL);
   strcpy(fpath,"/loader/dos/dosmod");
   execl(path,mm_name,NULL);
  }
  /* okay, it wasn't there, try in the path */
  execlp("dosmod",mm_name,NULL);
  /* last desperate attempts: current directory */
  execl("dosmod",mm_name,NULL);
  /* and, just for completeness... */
  execl("loader/dos/dosmod",mm_name,NULL);
  /* if failure, exit */
  ERR("Failed to spawn dosmod, error=%s\n",strerror(errno));
  exit(1);
 }
 return TRUE;
}

static void MZ_Launch(void)
{
  LPDOSTASK lpDosTask = MZ_Current();
  CONTEXT context;
  TDB *pTask = (TDB *)GlobalLock16( GetCurrentTask() );
  BYTE *psp_start = PTR_REAL_TO_LIN( lpDosTask->psp_seg, 0 );

  MZ_FillPSP(psp_start, GetCommandLineA());
  pTask->flags |= TDBF_WINOLDAP;

  memset( &context, 0, sizeof(context) );
  context.SegCs  = init_cs;
  context.Eip    = init_ip;
  context.SegSs  = init_ss;
  context.Esp    = init_sp;
  context.SegDs  = lpDosTask->psp_seg;
  context.SegEs  = lpDosTask->psp_seg;
  context.EFlags = 0x00080000;  /* virtual interrupt flag */
  DOSVM_Enter( &context );
}

static void MZ_KillTask(void)
{
  TRACE("killing DOS task\n");
  VGA_Clean();
  kill(dosmod_pid,SIGTERM);
}

void MZ_Exit( CONTEXT86 *context, BOOL cs_psp, WORD retval )
{
  LPDOSTASK lpDosTask = MZ_Current();
  if (lpDosTask) {
    WORD psp_seg = cs_psp ? context->SegCs : lpDosTask->psp_seg;
    LPBYTE psp_start = (LPBYTE)((DWORD)psp_seg << 4);
    PDB16 *psp = (PDB16 *)psp_start;
    WORD parpsp = psp->parentPSP; /* check for parent DOS process */
    if (parpsp) {
      /* retrieve parent's return address */
      FARPROC16 retaddr = INT_GetRMHandler(0x22);
      /* restore interrupts */
      INT_SetRMHandler(0x22, psp->savedint22);
      INT_SetRMHandler(0x23, psp->savedint23);
      INT_SetRMHandler(0x24, psp->savedint24);
      /* FIXME: deallocate file handles etc */
      /* free process's associated memory
       * FIXME: walk memory and deallocate all blocks owned by process */
      DOSMEM_FreeBlock(DOSMEM_MapRealToLinear(MAKELONG(0,psp->environment)));
      DOSMEM_FreeBlock(DOSMEM_MapRealToLinear(MAKELONG(0,lpDosTask->psp_seg)));
      /* switch to parent's PSP */
      lpDosTask->psp_seg = parpsp;
      psp_start = (LPBYTE)((DWORD)parpsp << 4);
      psp = (PDB16 *)psp_start;
      /* now return to parent */
      lpDosTask->retval = retval;
      context->SegCs = SELECTOROF(retaddr);
      context->Eip   = OFFSETOF(retaddr);
      context->SegSs = SELECTOROF(psp->saveStack);
      context->Esp   = OFFSETOF(psp->saveStack);
      return;
    } else
      MZ_KillTask();
  }
  ExitThread( retval );
}

#else /* !MZ_SUPPORTED */

BOOL MZ_LoadImage( LPCSTR cmdline )
{
  WARN("DOS executables not supported on this platform\n");
  SetLastError(ERROR_BAD_FORMAT);
  return FALSE;
}

BOOL MZ_Exec( CONTEXT86 *context, LPCSTR filename, BYTE func, LPVOID paramblk )
{
  /* can't happen */
  SetLastError(ERROR_BAD_FORMAT);
  return FALSE;
}

LPDOSTASK MZ_AllocDPMITask( void )
{
    ERR("Actual real-mode calls not supported on this platform!\n");
    return NULL;
}

void MZ_Exit( CONTEXT86 *context, BOOL cs_psp, WORD retval )
{
  ExitThread( retval );
}

#endif /* !MZ_SUPPORTED */

LPDOSTASK MZ_Current( void )
{
  return dos_current;
}
