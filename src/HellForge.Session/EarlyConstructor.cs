// The memory-only initialization edit a session makes on the copied game (generated from scripts/EarlyConstructorExperiment.cs,
// pinned 3C30CA3C..., by scripts/HostEarlyConstructor.Common.ps1 -SkipLaterStatusFailures -AnyHost, as a session did): kept
// as that code was compiled, with the copied exe it checks for (TargetExe) given by the session runner instead of baked in.
// Changed since, for a player whose every session ended "crt_already_started" (the game had started its C runtime, and
// went on to run GameGuard): once the constructor's bytes are decoded the boundary is polled every 0.25 ms rather than
// every 1-2 ms, and the whole game is frozen in one call (NtSuspendProcess) the moment the boundary is seen. Before, the
// game ran on until its threads were listed (a query of every process on the PC) and suspended one by one. The report
// also keeps when the bytes and the boundary were seen, and the C runtime's state right after the freeze call
// (CrtStateAtFreeze): 1 there means the game had started its C runtime before the boundary could be seen.
#nullable disable
#pragma warning disable
// Host copy adaptation of the pinned VM experiment. Memory edits only; fixed host and copied image.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

public sealed class HellForgeHostEarlyThread {
    public int Id; public string Rip, Rsp, Rbx;
}
public sealed class HellForgeHostEarlyResult {
    public string Mode, Outcome, Failure, ImageBase, ConstructorBefore, ConstructorAfter;
    public string MainCheckBefore, MainCheckAfter;
    public bool MainCheckWriteAttempted, MainCheckWritten;
    public bool LaterStatusWriteAttempted, LaterStatusWritten, LaterStatusRolledBack;
    public string LoopStatusBefore, LoopStatusAfter, FinalStatusBefore, FinalStatusAfter;
    public int ProcessId, Samples, ReadFailures, CrtState, CrtStateAtFreeze;
    public long ElapsedMilliseconds, PauseMilliseconds, BytesReadyMilliseconds, BoundaryMilliseconds;
    public bool ProcessFrozen, BytesReady, ConditionsMatched, WriteAttempted, Written, ReadbackMatched, RolledBack, Terminated;
    public List<HellForgeHostEarlyThread> Threads = new List<HellForgeHostEarlyThread>();
}
public static class HellForgeHostEarlyConstructorExperiment {
    // The copied game executable the process must be (set by the session runner).
    public static string TargetExe;
    [DllImport("kernel32.dll",SetLastError=true)] static extern IntPtr OpenProcess(uint access,bool inherit,int pid);
    [DllImport("kernel32.dll",SetLastError=true)] static extern IntPtr OpenThread(uint access,bool inherit,uint tid);
    [DllImport("kernel32.dll")] static extern uint GetProcessIdOfThread(IntPtr h);
    [DllImport("kernel32.dll",SetLastError=true)] static extern uint SuspendThread(IntPtr h);
    [DllImport("kernel32.dll",SetLastError=true)] static extern uint ResumeThread(IntPtr h);
    [DllImport("ntdll.dll")] static extern int NtSuspendProcess(IntPtr p);
    [DllImport("ntdll.dll")] static extern int NtResumeProcess(IntPtr p);
    [DllImport("kernel32.dll",SetLastError=true)] static extern bool GetThreadContext(IntPtr h,IntPtr context);
    [DllImport("kernel32.dll",SetLastError=true)] static extern bool ReadProcessMemory(IntPtr p,IntPtr a,byte[] b,UIntPtr n,out UIntPtr read);
    [DllImport("kernel32.dll",SetLastError=true)] static extern bool WriteProcessMemory(IntPtr p,IntPtr a,byte[] b,UIntPtr n,out UIntPtr written);
    [DllImport("kernel32.dll",SetLastError=true)] static extern bool VirtualProtectEx(IntPtr p,IntPtr a,UIntPtr n,uint protection,out uint previous);
    [DllImport("kernel32.dll",SetLastError=true)] static extern bool FlushInstructionCache(IntPtr p,IntPtr a,UIntPtr n);
    [DllImport("kernel32.dll",SetLastError=true,CharSet=CharSet.Unicode)] static extern bool QueryFullProcessImageName(IntPtr p,uint flags,StringBuilder path,ref int size);
    [DllImport("kernel32.dll")] static extern bool TerminateProcess(IntPtr p,uint code);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    [DllImport("winmm.dll")] static extern uint timeBeginPeriod(uint p);
    [DllImport("winmm.dll")] static extern uint timeEndPeriod(uint p);
    static Exception Error(string m) { return new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(),m); }
    static byte[] Hex(string s) { var b=new byte[s.Length/2];for(int i=0;i<b.Length;i++)b[i]=Convert.ToByte(s.Substring(i*2,2),16);return b; }
    static string Hex(byte[] b) { return BitConverter.ToString(b).Replace("-","").ToLowerInvariant(); }
    static byte[] Read(IntPtr p,long a,int n) {
        var b=new byte[n]; UIntPtr read;
        if(!ReadProcessMemory(p,new IntPtr(a),b,new UIntPtr((uint)n),out read)||read.ToUInt64()!=(ulong)n)throw Error("ReadProcessMemory");return b;
    }
    static void Write(IntPtr p,long a,byte value) {
        WriteBytes(p,a,new byte[]{value});
    }
    static void WriteBytes(IntPtr p,long a,byte[] value) {
        uint previous;var ptr=new IntPtr(a);var one=new UIntPtr((uint)value.Length);
        if(!VirtualProtectEx(p,ptr,one,0x40,out previous))throw Error("VirtualProtectEx");
        try { UIntPtr n;if(!WriteProcessMemory(p,ptr,value,one,out n)||n.ToUInt64()!=(ulong)value.Length)throw Error("WriteProcessMemory");if(!FlushInstructionCache(p,ptr,one))throw Error("FlushInstructionCache"); }
        finally {uint ignored;if(!VirtualProtectEx(p,ptr,one,previous,out ignored))throw Error("Restore protection");}
        if(Hex(Read(p,a,value.Length))!=Hex(value))throw new InvalidOperationException("Patch readback mismatch.");
    }
    public static HellForgeHostEarlyResult ApplyGame(int pid,long startTicks,string mode) {return Apply(pid,startTicks,0,mode,false,false);}
    public static HellForgeHostEarlyResult ApplyFixture(int pid,long startTicks,long allocation,string mode) {return Apply(pid,startTicks,allocation,mode,true,false);}
    public static HellForgeHostEarlyResult ApplySteamGame(int pid,long startTicks,string mode) {return Apply(pid,startTicks,0,mode,false,true);}
    public static HellForgeHostEarlyResult ApplySteamFixture(int pid,long startTicks,long allocation,string mode) {return Apply(pid,startTicks,allocation,mode,true,true);}
    static HellForgeHostEarlyResult Apply(int pid,long startTicks,long allocation,string mode,bool fixture,bool steamGuest) {
        if(false||IntPtr.Size!=8||pid==Process.GetCurrentProcess().Id)throw new InvalidOperationException("External guest x64 target required.");
        if(mode!="SameBytes"&&mode!="SkipConstructor"&&mode!="SkipConstructorAndMainCheck")throw new ArgumentException("Unknown fixed mode.");
        if(fixture&&allocation<=0)throw new ArgumentException("A valid fixture allocation is required.");
        var r=new HellForgeHostEarlyResult{ProcessId=pid,Mode=mode,Outcome="not_applied",CrtState=-1,CrtStateAtFreeze=-1,BytesReadyMilliseconds=-1,BoundaryMilliseconds=-1};
        var clock=Stopwatch.StartNew();var paused=new Stopwatch();var threads=new Dictionary<int,IntPtr>();
        IntPtr target=IntPtr.Zero;Process process=null;bool timer=false,frozen=false;long imageBase=allocation;
        int ctor=fixture?0:0x24b30, state=fixture?64:0x19b1e40, slot=fixture?80:0x14002d0, obj=fixture?72:0x1982e90;
        bool editMain=mode=="SkipConstructorAndMainCheck";
        int mainCheck=fixture?138:0x5a53e;
        if(fixture)throw new InvalidOperationException("Later-status adaptation is host-only.");
        byte[] loopStatusBefore=Hex("e80e908e003d550700000f84b4000000");
        byte[] finalStatusBefore=Hex("e8628591003d550700007570");
        byte[] loopStatusPatch=Hex("e9b500000090"), finalStatusPatch=Hex("9090");
        byte[] mainBefore=Hex(fixture?"0f8506000000":"0f85ea000000"),mainAfter=Hex("909090909090");
        byte[] expected=Hex(fixture?"c6051900000001c3":"4883ec28488d0d7dbb6201e894e89400488d0d89c23c014883c428e988ef2301");
        try {
            target=OpenProcess(0x1839,false,pid);if(target==IntPtr.Zero)throw Error("OpenProcess");
            var path=new StringBuilder(1024);int length=path.Capacity;
            if(!QueryFullProcessImageName(target,0,path,ref length))throw Error("Process identity");
            if(fixture)throw new InvalidOperationException("Host fixture entry disabled."); string wanted=TargetExe??throw new InvalidOperationException("No target executable.");
            if(!String.Equals(path.ToString(),wanted,StringComparison.OrdinalIgnoreCase))throw new InvalidOperationException("Unexpected process image.");
            process=Process.GetProcessById(pid);
            if(process.HasExited||process.StartTime.Ticks!=startTicks)throw new InvalidOperationException("Process start identity changed.");
            timer=timeBeginPeriod(1)==0;bool boundaryReady=false;
            while(clock.ElapsedMilliseconds<(fixture?1200:15000)) {
                r.Samples++;
                if(process.HasExited){r.Outcome="target_exited_before_boundary";return r;}
                try {
                    if(!fixture&&imageBase==0){try{process.Refresh();var mainModule=process.MainModule;if(mainModule!=null)imageBase=mainModule.BaseAddress.ToInt64();}catch(NullReferenceException moduleError){r.ReadFailures++;r.Failure="Transient module query: "+moduleError.ToString();}}
                    if(imageBase!=0&&Hex(Read(target,imageBase+ctor,expected.Length))==Hex(expected)) {
                        if(!r.BytesReady){r.BytesReady=true;r.BytesReadyMilliseconds=clock.ElapsedMilliseconds;}
                        if(BitConverter.ToInt64(Read(target,imageBase+slot,8),0)!=imageBase+ctor)r.Outcome="constructor_table_not_ready";
                        else if(!fixture&&Hex(Read(target,imageBase+0x973004,20))!="488b0d85fe00014885c9750333c0c3e9a890ffff")r.Outcome="main_wrapper_not_ready";
                        else if(editMain&&Hex(Read(target,imageBase+mainCheck,mainBefore.Length))!=Hex(mainBefore))r.Outcome="main_check_not_ready";
                        else {boundaryReady=true;r.BoundaryMilliseconds=clock.ElapsedMilliseconds;break;}
                    }
                }catch(System.ComponentModel.Win32Exception){r.ReadFailures++;}
                // From the decoded bytes on, poll every 0.25 ms (a busy wait on this thread) rather than sleeping 1-2 ms,
                // so the boundary is seen sooner. Not from the start: under Wine each read is a wineserver request, and
                // the game's own requests queue behind them.
                if(r.BytesReady){long until=Stopwatch.GetTimestamp()+Stopwatch.Frequency/4000;while(Stopwatch.GetTimestamp()<until)Thread.SpinWait(16);}
                else Thread.Sleep(1);
            }
            r.ImageBase="0x"+imageBase.ToString("x");
            if(!boundaryReady){if(!r.BytesReady)r.Outcome="loaded_bytes_not_observed";return r;}
            // Freeze the whole game at once (every thread, in one call), so the slow thread listing below cannot let it
            // run on; then freeze a stable thread set, and recheck all assumptions atomically with respect to those threads.
            paused.Start();
            int suspended=NtSuspendProcess(target);if(suspended<0)throw new InvalidOperationException("NtSuspendProcess failed: 0x"+suspended.ToString("x8"));
            frozen=true;r.ProcessFrozen=true;
            r.CrtStateAtFreeze=BitConverter.ToInt32(Read(target,imageBase+state,4),0);
            bool stable=false;
            for(int pass=0;pass<8&&paused.ElapsedMilliseconds<1500;pass++) {
                int added=0;process.Refresh();
                foreach(ProcessThread t in process.Threads) {
                    if(threads.ContainsKey(t.Id))continue;
                    IntPtr h=OpenThread(0x80a,false,(uint)t.Id);if(h==IntPtr.Zero)throw Error("OpenThread");
                    if(GetProcessIdOfThread(h)!=(uint)pid){CloseHandle(h);throw new InvalidOperationException("Thread owner changed.");}
                    if(SuspendThread(h)==uint.MaxValue){CloseHandle(h);throw Error("SuspendThread");}
                    threads.Add(t.Id,h);added++;
                    IntPtr raw=Marshal.AllocHGlobal(1248);
                    try {
                        IntPtr context=new IntPtr((raw.ToInt64()+15)&~15L);Marshal.Copy(new byte[1232],0,context,1232);
                        Marshal.WriteInt32(context,48,0x100003);
                        if(!GetThreadContext(h,context))throw Error("GetThreadContext");
                        long rip=Marshal.ReadInt64(context,248);
                        if((rip>=imageBase+0x89b55&&rip<imageBase+0x89b65)||(rip>=imageBase+0x5a601&&rip<imageBase+0x5a60d))throw new InvalidOperationException("Thread executing later status-check site.");
                        r.Threads.Add(new HellForgeHostEarlyThread{Id=t.Id,Rip="0x"+rip.ToString("x"),Rsp="0x"+Marshal.ReadInt64(context,152).ToString("x"),Rbx="0x"+Marshal.ReadInt64(context,144).ToString("x")});
                        if(rip>=imageBase+ctor&&rip<imageBase+ctor+expected.Length)throw new InvalidOperationException("Thread executing constructor site.");
                        if(editMain&&rip>=imageBase+mainCheck&&rip<imageBase+mainCheck+mainBefore.Length)throw new InvalidOperationException("Thread executing main check site.");
                    }finally{Marshal.FreeHGlobal(raw);}
                }
                if(added==0&&threads.Count>0){stable=true;break;}
            }
            if(!stable)throw new TimeoutException("Stable thread-set deadline exceeded.");
            r.ConstructorBefore=Hex(Read(target,imageBase+ctor,expected.Length));
            if(r.ConstructorBefore!=Hex(expected)){r.Outcome="loaded_bytes_changed";return r;}
            r.CrtState=BitConverter.ToInt32(Read(target,imageBase+state,4),0);
            if(r.CrtState!=0){r.Outcome="crt_already_started";return r;}
            if(BitConverter.ToInt64(Read(target,imageBase+obj,8),0)!=0){r.Outcome="runtime_object_already_present";return r;}
            if(BitConverter.ToInt64(Read(target,imageBase+slot,8),0)!=imageBase+ctor){r.Outcome="constructor_table_not_ready";return r;}
            if(!fixture) {
                // The main wrapper must also have its original decoded bytes before editing.
                if(Hex(Read(target,imageBase+0x973004,20))!="488b0d85fe00014885c9750333c0c3e9a890ffff"){r.Outcome="main_wrapper_not_ready";return r;}
            }
            if(editMain) {
                r.MainCheckBefore=Hex(Read(target,imageBase+mainCheck,mainBefore.Length));
                if(r.MainCheckBefore!=Hex(mainBefore)){r.Outcome="main_check_changed";return r;}
            }
            r.LoopStatusBefore=Hex(Read(target,imageBase+0x89b55,loopStatusBefore.Length));
            r.FinalStatusBefore=Hex(Read(target,imageBase+0x5a601,finalStatusBefore.Length));
            if(r.LoopStatusBefore!=Hex(loopStatusBefore)||r.FinalStatusBefore!=Hex(finalStatusBefore)){r.Outcome="later_status_bytes_mismatch";return r;}
            r.ConditionsMatched=true;r.WriteAttempted=true;
            Write(target,imageBase+ctor,mode=="SameBytes"?expected[0]:(byte)0xc3);r.Written=true;
            byte[] after=(byte[])expected.Clone();after[0]=mode=="SameBytes"?expected[0]:(byte)0xc3;
            r.ConstructorAfter=Hex(Read(target,imageBase+ctor,expected.Length));
            if(r.ConstructorAfter!=Hex(after))throw new InvalidOperationException("Full constructor readback changed.");
            if(editMain) {
                r.MainCheckWriteAttempted=true;WriteBytes(target,imageBase+mainCheck,mainAfter);r.MainCheckWritten=true;
                r.MainCheckAfter=Hex(Read(target,imageBase+mainCheck,mainAfter.Length));
                if(r.MainCheckAfter!=Hex(mainAfter))throw new InvalidOperationException("Main check readback mismatch.");
                if(Hex(Read(target,imageBase+ctor,expected.Length))!=Hex(after))throw new InvalidOperationException("Constructor changed during main edit.");
            }
            r.LaterStatusWriteAttempted=true;
            WriteBytes(target,imageBase+0x89b5f,loopStatusPatch);
            WriteBytes(target,imageBase+0x5a60b,finalStatusPatch);
            r.LoopStatusAfter=Hex(Read(target,imageBase+0x89b55,loopStatusBefore.Length));
            r.FinalStatusAfter=Hex(Read(target,imageBase+0x5a601,finalStatusBefore.Length));
            if(r.LoopStatusAfter!="e80e908e003d55070000e9b500000090"||r.FinalStatusAfter!="e8628591003d550700009090")throw new InvalidOperationException("Later status full-window readback mismatch.");
            r.LaterStatusWritten=true;
            r.ReadbackMatched=true;r.Outcome="applied_at_candidate_boundary";
        }catch(Exception e) {
            r.Failure=e.ToString();r.Outcome="experiment_failed";
            if(r.WriteAttempted)try{if(r.LaterStatusWriteAttempted){WriteBytes(target,imageBase+0x89b5f,Hex("0f84b4000000"));WriteBytes(target,imageBase+0x5a60b,Hex("7570"));r.LaterStatusRolledBack=true;}if(r.MainCheckWriteAttempted)WriteBytes(target,imageBase+mainCheck,mainBefore);Write(target,imageBase+ctor,expected[0]);r.RolledBack=true;}catch{r.Terminated=TerminateProcess(target,0xb7510001);}
        }finally {
            foreach(IntPtr h in threads.Values){if(ResumeThread(h)==uint.MaxValue){r.Failure+="; resume failed";r.Outcome="experiment_failed";r.Terminated=TerminateProcess(target,0xb7510002);}CloseHandle(h);}
            if(frozen&&NtResumeProcess(target)<0){r.Failure+="; process resume failed";r.Outcome="experiment_failed";r.Terminated=TerminateProcess(target,0xb7510003);}
            paused.Stop();r.PauseMilliseconds=paused.ElapsedMilliseconds;
            if(timer)timeEndPeriod(1);if(process!=null)process.Dispose();if(target!=IntPtr.Zero)CloseHandle(target);
            r.ElapsedMilliseconds=clock.ElapsedMilliseconds;
        }
        return r;
    }
}
