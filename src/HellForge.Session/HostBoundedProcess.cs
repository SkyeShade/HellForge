// From scripts/HostBoundedProcess.cs, as sessions compiled it.
#nullable disable
#pragma warning disable
// Native host experiment: create suspended, assign to kill-on-close job, then resume.
using System;
using System.Collections;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

public sealed class HostBoundedProcess : IDisposable {
    IntPtr job, thread; Timer deadline; public Process Process {get;private set;}
    public bool DeadlineExpired {get;private set;}
    // singleProcess: the job holds the game alone (an active-process limit of 1), so it cannot start other programs.
    // token: when non-zero, CreateProcessAsUser with this token (a lowered integrity level); the caller owns the handle.
    public HostBoundedProcess(string executable,string arguments,string workingDirectory,string profile,int seconds,bool singleProcess=false,IntPtr token=default) {
        // 0 means no deadline (play sessions); the job still kills the game when its owner exits.
        if(seconds<0||seconds>330)throw new ArgumentOutOfRangeException("seconds");
        var pi=new PI();IntPtr env=IntPtr.Zero;
        try {
            job=CreateJobObjectW(IntPtr.Zero,null);if(job==IntPtr.Zero)Fail("CreateJobObject");
            var limits=new LIMIT();limits.Basic.Flags=0x2000;
            if(singleProcess){limits.Basic.Flags|=0x8;limits.Basic.ActiveLimit=1;}
            if(!SetInformationJobObject(job,9,ref limits,Marshal.SizeOf(typeof(LIMIT))))Fail("Job limits");
            var variables=new SortedDictionary<string,string>(StringComparer.OrdinalIgnoreCase);
            foreach(DictionaryEntry item in Environment.GetEnvironmentVariables())variables[(string)item.Key]=(string)item.Value;
            if(!String.IsNullOrEmpty(profile)) {
                variables["APPDATA"]=System.IO.Path.Combine(profile,"Roaming");
                variables["LOCALAPPDATA"]=System.IO.Path.Combine(profile,"Local");
                variables["TEMP"]=variables["TMP"]=System.IO.Path.Combine(profile,"Temp");
            }
            var pairs=new List<string>();foreach(var item in variables)pairs.Add(item.Key+"="+item.Value);
            env=Marshal.StringToHGlobalUni(String.Join("\0",pairs.ToArray())+"\0\0");
            var si=new SI();si.Size=Marshal.SizeOf(typeof(SI));
            var cmd=new StringBuilder("\""+executable+"\" "+arguments);
            if(token!=IntPtr.Zero){if(!CreateProcessAsUserW(token,executable,cmd,IntPtr.Zero,IntPtr.Zero,false,0x404,env,workingDirectory,ref si,out pi))Fail("Create suspended (low integrity)");}
            else{if(!CreateProcessW(executable,cmd,IntPtr.Zero,IntPtr.Zero,false,0x404,env,workingDirectory,ref si,out pi))Fail("Create suspended");}
            thread=pi.Thread;
            if(!AssignProcessToJobObject(job,pi.Process))Fail("Assign suspended process");
            bool member;if(!IsProcessInJob(pi.Process,job,out member)||!member)throw new InvalidOperationException("Job membership not verified");
            Process=Process.GetProcessById((int)pi.Pid);var retained=Process.Handle;
            if(seconds>0)deadline=new Timer(delegate(object unused){DeadlineExpired=true;TerminateJobObject(job,0xb7520001);},null,seconds*1000,Timeout.Infinite);
        }catch{if(pi.Process!=IntPtr.Zero)TerminateProcess(pi.Process,0xb7520002);Dispose();throw;}
        finally{if(pi.Process!=IntPtr.Zero)CloseHandle(pi.Process);if(env!=IntPtr.Zero)Marshal.FreeHGlobal(env);}
    }
    public void Resume(){if(thread==IntPtr.Zero)throw new InvalidOperationException("Already resumed");if(ResumeThread(thread)==UInt32.MaxValue)Fail("Resume");CloseHandle(thread);thread=IntPtr.Zero;}
    public uint ActiveProcesses {get{var info=new ACCOUNTING();uint returned;if(!QueryInformationJobObject(job,1,ref info,Marshal.SizeOf(typeof(ACCOUNTING)),out returned))Fail("Query job");return info.Active;}}
    public void Stop(){if(job!=IntPtr.Zero&&!TerminateJobObject(job,0xb7520003))Fail("Terminate job");}
    public void Dispose(){if(deadline!=null){using(var done=new ManualResetEvent(false)){if(deadline.Dispose(done))done.WaitOne();}deadline=null;}if(job!=IntPtr.Zero){TerminateJobObject(job,0xb7520004);CloseHandle(job);job=IntPtr.Zero;}if(thread!=IntPtr.Zero){CloseHandle(thread);thread=IntPtr.Zero;}if(Process!=null){Process.Dispose();Process=null;}}
    static void Fail(string context){throw new Win32Exception(Marshal.GetLastWin32Error(),context);}
    [StructLayout(LayoutKind.Sequential)]struct PI{public IntPtr Process,Thread;public uint Pid,Tid;}
    [StructLayout(LayoutKind.Sequential,CharSet=CharSet.Unicode)]struct SI{public int Size;public string Reserved,Desktop,Title;public uint X,Y,XSize,YSize,XChars,YChars,Fill,Flags;public short Show,Reserved2Size;public IntPtr Reserved2,Input,Output,Error;}
    [StructLayout(LayoutKind.Sequential)]struct BASIC{public long ProcessTime,JobTime;public uint Flags;public UIntPtr MinWorkingSet,MaxWorkingSet;public uint ActiveLimit;public UIntPtr Affinity;public uint Priority,Scheduling;}
    [StructLayout(LayoutKind.Sequential)]struct IO{public ulong ReadOps,WriteOps,OtherOps,ReadBytes,WriteBytes,OtherBytes;}
    [StructLayout(LayoutKind.Sequential)]struct LIMIT{public BASIC Basic;public IO Io;public UIntPtr ProcessMemory,JobMemory,PeakProcessMemory,PeakJobMemory;}
    [StructLayout(LayoutKind.Sequential)]struct ACCOUNTING{public long UserTime,KernelTime,PeriodUser,PeriodKernel;public uint PageFaults,Total,Active,Terminated;}
    [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)]static extern bool CreateProcessW(string app,StringBuilder command,IntPtr pa,IntPtr ta,bool inherit,uint flags,IntPtr env,string cwd,ref SI si,out PI pi);
    [DllImport("advapi32.dll",CharSet=CharSet.Unicode,SetLastError=true)]static extern bool CreateProcessAsUserW(IntPtr token,string app,StringBuilder command,IntPtr pa,IntPtr ta,bool inherit,uint flags,IntPtr env,string cwd,ref SI si,out PI pi);
    [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)]static extern IntPtr CreateJobObjectW(IntPtr attrs,string name);
    [DllImport("kernel32.dll",SetLastError=true)]static extern bool SetInformationJobObject(IntPtr job,int cls,ref LIMIT info,int size);
    [DllImport("kernel32.dll",SetLastError=true)]static extern bool QueryInformationJobObject(IntPtr job,int cls,ref ACCOUNTING info,int size,out uint returned);
    [DllImport("kernel32.dll",SetLastError=true)]static extern bool AssignProcessToJobObject(IntPtr job,IntPtr process);
    [DllImport("kernel32.dll",SetLastError=true)]static extern bool IsProcessInJob(IntPtr process,IntPtr job,out bool member);
    [DllImport("kernel32.dll",SetLastError=true)]static extern uint ResumeThread(IntPtr thread);
    [DllImport("kernel32.dll",SetLastError=true)]static extern bool TerminateJobObject(IntPtr job,uint code);
    [DllImport("kernel32.dll",SetLastError=true)]static extern bool TerminateProcess(IntPtr process,uint code);
    [DllImport("kernel32.dll")]static extern bool CloseHandle(IntPtr handle);
}
