// From scripts/LocalRoutingPatch.cs, as sessions compiled it.
#nullable disable
#pragma warning disable
// Host-only memory edit: route the copied game's backend selection to a loopback origin.
// Never edits game files. Waits for WinLicense to decode game.dll, then writes only if the
// pinned original bytes are present. Constants are exercised by ProbeLocalRoutingPatch.py.
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
public class HellForgeLocalRoutingThread { public int Id; public string Rip; }
public class HellForgeLocalRoutingResult {
    public int ProcessId; public string Url; public string Outcome; public string Failure;
    public string DllBase; public long Samples, ReadFailures, ModuleSeenMilliseconds=-1, ElapsedMilliseconds, PauseMilliseconds;
    public string SiteBefore, SiteAfter, CaveAfter;
    public bool WriteAttempted, CaveWritten, SiteWritten, ReadbackMatched, RolledBack, Terminated;
    public int GateBranchesWritten; public bool GateReadbackMatched;
    public List<HellForgeLocalRoutingThread> Threads=new List<HellForgeLocalRoutingThread>();
}
public static class HellForgeLocalRoutingPatch {
    [DllImport("kernel32.dll",SetLastError=true)] static extern IntPtr OpenProcess(uint access,bool inherit,int pid);
    [DllImport("kernel32.dll",SetLastError=true)] static extern IntPtr OpenThread(uint access,bool inherit,uint tid);
    [DllImport("kernel32.dll",SetLastError=true)] static extern uint GetProcessIdOfThread(IntPtr thread);
    [DllImport("kernel32.dll",SetLastError=true)] static extern uint SuspendThread(IntPtr thread);
    [DllImport("kernel32.dll",SetLastError=true)] static extern uint ResumeThread(IntPtr thread);
    [DllImport("kernel32.dll",SetLastError=true)] static extern bool GetThreadContext(IntPtr thread,IntPtr context);
    [DllImport("kernel32.dll",SetLastError=true)] static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll",SetLastError=true)] static extern bool TerminateProcess(IntPtr p,uint code);
    [DllImport("kernel32.dll",SetLastError=true)] static extern bool GetExitCodeProcess(IntPtr p,out uint code);
    [DllImport("kernel32.dll",SetLastError=true,CharSet=CharSet.Unicode)] static extern bool QueryFullProcessImageName(IntPtr p,int flags,StringBuilder name,ref int size);
    [DllImport("kernel32.dll",SetLastError=true)] static extern bool ReadProcessMemory(IntPtr p,IntPtr a,byte[] b,UIntPtr n,out UIntPtr read);
    [DllImport("kernel32.dll",SetLastError=true)] static extern bool WriteProcessMemory(IntPtr p,IntPtr a,byte[] b,UIntPtr n,out UIntPtr written);
    [DllImport("kernel32.dll",SetLastError=true)] static extern bool VirtualProtectEx(IntPtr p,IntPtr a,UIntPtr n,uint protection,out uint previous);
    [DllImport("kernel32.dll",SetLastError=true)] static extern bool FlushInstructionCache(IntPtr p,IntPtr a,UIntPtr n);
    [DllImport("psapi.dll",SetLastError=true)] static extern bool EnumProcessModulesEx(IntPtr p,[Out] IntPtr[] modules,int size,out int needed,uint filter);
    [DllImport("psapi.dll",SetLastError=true,CharSet=CharSet.Unicode)] static extern uint GetModuleFileNameEx(IntPtr p,IntPtr module,StringBuilder name,int size);
    [DllImport("winmm.dll")] static extern uint timeBeginPeriod(uint ms);
    [DllImport("winmm.dll")] static extern uint timeEndPeriod(uint ms);

    const int Site=0x12b663d, Cave=0x1af7680, CaveSize=64;
    // jne preset; mov rdx,[settings]; add rdx,0x3e4  ->  nop; lea rdx,[rip+cave]; nop
    static readonly byte[] SiteBefore=Hex("7575488b15fafc06024881c2e4030000");
    static readonly byte[] SiteAfter=Hex("6690488d153a1084000f1f8000000000");
    // Backend-enable gate 0xab4663..0xab4787 (GateEnd 0xab4787). It enables the manager from the
    // genuine signed-in state (global X: X+8==0 && X+0x2492!=0); when cfg+0x28!=0 it also requires
    // the platform-matchmaking service (*(dll+0x3326308)+0xf8) which is not constructed offline.
    // Both cfg+0x28 branches je->jmp take the "no platform matchmaking" path, so the manager enables
    // from the real sign-in with no public servers. This does not fabricate sign-in (verified by
    // scripts/ProbeLocalGatePatch.py: not-signed-in stays disabled). Original bytes checked first.
    static readonly int[] GateSites={0xab4714,0xab4743};
    static readonly byte[][] GateBefore={Hex("7411"),Hex("7415")};
    static readonly byte[][] GateAfter={Hex("eb11"),Hex("eb15")};

    static byte[] Hex(string s){var b=new byte[s.Length/2];for(int i=0;i<b.Length;i++)b[i]=Convert.ToByte(s.Substring(i*2,2),16);return b;}
    static string Hex(byte[] b){return BitConverter.ToString(b).Replace("-","").ToLowerInvariant();}
    static Win32Exception Error(string what){return new Win32Exception(Marshal.GetLastWin32Error(),what);}
    static byte[] Read(IntPtr p,long a,int n){var b=new byte[n];UIntPtr read;
        if(!ReadProcessMemory(p,new IntPtr(a),b,new UIntPtr((uint)n),out read)||read.ToUInt64()!=(ulong)n)throw Error("ReadProcessMemory");return b;}
    static void WriteBytes(IntPtr p,long a,byte[] value){
        IntPtr ptr=new IntPtr(a);UIntPtr size=new UIntPtr((uint)value.Length);uint previous;
        if(!VirtualProtectEx(p,ptr,size,0x40,out previous))throw Error("VirtualProtectEx");
        try { UIntPtr n;if(!WriteProcessMemory(p,ptr,value,size,out n)||n.ToUInt64()!=(ulong)value.Length)throw Error("WriteProcessMemory");if(!FlushInstructionCache(p,ptr,size))throw Error("FlushInstructionCache"); }
        finally {uint ignored;if(!VirtualProtectEx(p,ptr,size,previous,out ignored))throw Error("Restore protection");}
    }
    static long FindModule(IntPtr p,string wanted){
        var modules=new IntPtr[1024];int needed;
        if(!EnumProcessModulesEx(p,modules,modules.Length*IntPtr.Size,out needed,0x02))throw Error("EnumProcessModulesEx");
        var name=new StringBuilder(1024);
        for(int i=0;i<Math.Min(needed/IntPtr.Size,modules.Length);i++){
            name.Length=0;if(GetModuleFileNameEx(p,modules[i],name,name.Capacity)==0)continue;
            if(String.Equals(name.ToString(),wanted,StringComparison.OrdinalIgnoreCase))return modules[i].ToInt64();
        }
        return 0;
    }
    static string Locate(IntPtr p,long address){
        var modules=new IntPtr[1024];int needed;
        if(!EnumProcessModulesEx(p,modules,modules.Length*IntPtr.Size,out needed,0x02))return "0x"+address.ToString("x");
        long best=0;string bestName=null;var name=new StringBuilder(1024);
        for(int i=0;i<Math.Min(needed/IntPtr.Size,modules.Length);i++){
            long b=modules[i].ToInt64();
            if(b<=address&&b>best){name.Length=0;if(GetModuleFileNameEx(p,modules[i],name,name.Capacity)!=0){best=b;bestName=System.IO.Path.GetFileName(name.ToString());}}
        }
        return bestName==null?"0x"+address.ToString("x"):bestName+"+0x"+(address-best).ToString("x");
    }
    static byte[] CaveBytes(string url){
        var m=Regex.Match(url??"","^http://127\\.0\\.0\\.1:([0-9]{1,5})$");int port;
        if(!m.Success||!int.TryParse(m.Groups[1].Value,out port)||port<1||port>65535)throw new ArgumentException("Only an http://127.0.0.1:<port> origin is accepted.");
        var raw=Encoding.ASCII.GetBytes(url+"\0");if(raw.Length>CaveSize)throw new ArgumentException("URL exceeds cave.");return raw;
    }
    static bool IsPadding(byte[] b){foreach(byte x in b)if(x!=0xcc)return false;return true;}

    // Read-only check that the edit is still present, e.g. after a long observation.
    public static string Inspect(int pid,string expectedDll){
        IntPtr target=OpenProcess(0x0410,false,pid);if(target==IntPtr.Zero)throw Error("OpenProcess");
        try {
            long dll=FindModule(target,expectedDll);if(dll==0)return "module_not_loaded";
            string site=Hex(Read(target,dll+Site,SiteBefore.Length));
            return site==Hex(SiteAfter)?"patched":site==Hex(SiteBefore)?"original":"other:"+site;
        }finally{CloseHandle(target);}
    }
    // Read-only backend state: root=*(dll+0x3326340); manager=root+0x1024220+0x1545598 (traced
    // through 0xab5000 -> 0x13f76e0 -> 0x12b5710 -> state machine 0x12b6570).
    // Read-only: item catalogue state at P=[dll+0x347CEF8]. Finalize 0x136e6a0 demotes kind-10 records whose itemId has
    // no StratagemInfo row (rows [dll+0x37CB608], types 1..149) to category 0; the per-category index starts at P+0xD1CE4.
    public static string DiagnoseCatalogue(int pid,string expectedDll){
        IntPtr target=OpenProcess(0x0410,false,pid);if(target==IntPtr.Zero)throw Error("OpenProcess");
        try {
            long dll=FindModule(target,expectedDll);if(dll==0)return "module_not_loaded";
            long p=BitConverter.ToInt64(Read(target,dll+0x347CEF8,8),0);if(p==0)return "catalogue=0";
            uint count=BitConverter.ToUInt32(Read(target,p+0x1CE0,4),0);
            byte[] starts=Read(target,p+0xD1CE4,4*24);
            var cats=new System.Text.StringBuilder();
            for(int c=0;c<23;c++)cats.Append(BitConverter.ToUInt32(starts,(c+1)*4)-BitConverter.ToUInt32(starts,c*4)).Append(c<22?",":"");
            byte[] rows=Read(target,dll+0x37CB608,8*149);int present=0;
            int[] bits=new int[16*8];
            for(int i=0;i<149;i++){long row=BitConverter.ToInt64(rows,i*8);if(row==0)continue;present++;
                byte[] flags=Read(target,row+0x80,0x48);
                for(int b=0;b<8;b++){if((flags[0]>>b&1)!=0)bits[b]++;if((flags[0x40]>>b&1)!=0)bits[8+b]++;}}
            return String.Format("items={0} progression={1} kind10=[{2},{3}) stratagemRows={4}/149 rowBits80={5} rowBitsC0={6} perCategory={7}",count,
                BitConverter.ToUInt32(Read(target,p+0xDDCF8,4),0),BitConverter.ToUInt32(starts,40),BitConverter.ToUInt32(starts,44),present,
                String.Join(",",Array.ConvertAll(bits,v=>v.ToString()),0,8),String.Join(",",Array.ConvertAll(bits,v=>v.ToString()),8,8),cats);
        }catch(Win32Exception e){return "read_failed:"+e.Message;}
        finally{CloseHandle(target);}
    }
    public static string Diagnose(int pid,string expectedDll){
        IntPtr target=OpenProcess(0x0410,false,pid);if(target==IntPtr.Zero)throw Error("OpenProcess");
        try {
            long dll=FindModule(target,expectedDll);if(dll==0)return "module_not_loaded";
            long root=BitConverter.ToInt64(Read(target,dll+0x3326340,8),0);if(root==0)return "root=0";
            long m=root+0x1024220+0x1545598;
            long gateGlobal=BitConverter.ToInt64(Read(target,dll+0x3326e40,8),0);
            long state0Global=BitConverter.ToInt64(Read(target,dll+0x347cdd8,8),0);
            string url=Encoding.ASCII.GetString(Read(target,m+0x44,64)).Split('\0')[0];
            // Writer of root+0x10b4 at 0xab46f8/0xab475a: needs X+8==0, X+0x2492!=0 and, when
            // cfg+0x28 is set, platform service (*(dll+0x3326308)+0xf8) vtable slot 1 returning true.
            long x=BitConverter.ToInt64(Read(target,dll+0x3326e48,8),0);
            long cfg=BitConverter.ToInt64(Read(target,dll+0x3326348,8),0);
            long platform=BitConverter.ToInt64(Read(target,dll+0x3326308,8),0);
            long service=platform==0?0:BitConverter.ToInt64(Read(target,platform+0xf8,8),0);
            long slot=service==0?0:BitConverter.ToInt64(Read(target,BitConverter.ToInt64(Read(target,service,8),0)+8,8),0);
            string enable=String.Format(" X+8={0} X+2492={1} cfg+28={2} root+10be={3} serviceSlot1={4}",
                x==0?-1:Read(target,x+8,1)[0],x==0?-1:Read(target,x+0x2492,1)[0],cfg==0?-1:Read(target,cfg+0x28,1)[0],
                Read(target,root+0x10be,1)[0],slot==0?"null":Locate(target,slot));
            return enable+" "+String.Format("state={0} prev={1} routing={2} url={3} gate[root+10b4]={4} gate[m+702f8]={5} gate[m+1]={6} gateMode[+4]={7} root+10bd={8} s0flag={9} s1[root+8]={10:x} s1[m+31dbd]={11}",
                BitConverter.ToInt32(Read(target,m+0x702fc,4),0),BitConverter.ToInt32(Read(target,m+0x70304,4),0),
                BitConverter.ToInt32(Read(target,m+0x444,4),0),url,Read(target,root+0x10b4,1)[0],Read(target,m+0x702f8,1)[0],
                Read(target,m+1,1)[0],gateGlobal==0?-1:BitConverter.ToInt32(Read(target,gateGlobal+4,4),0),Read(target,root+0x10bd,1)[0],
                state0Global==0?-1:Read(target,state0Global+0xc,1)[0],BitConverter.ToInt64(Read(target,root+8,8),0),Read(target,m+0x31dbd,1)[0]);
        }catch(Win32Exception e){return "read_failed:"+e.Message;}
        finally{CloseHandle(target);}
    }
    // DATA-only: clear the cfg+0x28 "platform matchmaking required" flag in the heap object
    // *(dll+0x3326348)+0x28, so the enable gate 0xab4663 skips the platform service and enables the
    // backend manager from the genuine sign-in. This writes one heap byte, never protected code, so
    // WinLicense integrity checks do not apply (contrast the opt-in gate code patch). The signed-in
    // state (global X) is untouched. Call repeatedly to hold it clear while the gate re-evaluates.
    public static string ClearPlatformMatchmaking(int pid,string expectedDll){
        IntPtr target=OpenProcess(0x0438,false,pid);if(target==IntPtr.Zero)throw Error("OpenProcess");
        try{
            long dll=FindModule(target,expectedDll);if(dll==0)return "module_not_loaded";
            long cfg=BitConverter.ToInt64(Read(target,dll+0x3326348,8),0);if(cfg==0)return "cfg_not_constructed";
            byte before=Read(target,cfg+0x28,1)[0];
            if(before==0)return "already_clear";
            WriteBytes(target,cfg+0x28,new byte[]{0});
            return Read(target,cfg+0x28,1)[0]==0?("cleared_from_"+before):"write_failed";
        }finally{CloseHandle(target);}
    }
    // PlayFab web API (engine, helldivers2.exe): the system object is [[[exe+0x235E3C8]+0x5D0]+0xA0]; its web API object
    // at +0x50 keeps the base URL "https://<title>.playfabapi.com" in a 0x80-byte buffer at +0x90 (init 0x8C4127..0x8C416D,
    // snprintf then "PlayfabWebApi initiated"). Every web request is "%s/Client/..." on that buffer. System +0x20 holds the
    // title id, +0x28 the logged-in byte (T slot 1, 0x8C5CF0), +0xB0 the title player account id (the peer id).
    const int PfGlobal=0x235E3C8;
    static readonly int[] PfPinSites={0x8C5CF0,0x8C40CE,0x8C416D};
    static readonly byte[][] PfPins={Hex("488b05d186a901488b88d0050000488b81a0000000"),Hex("488d9f90000000"),Hex("48897e50")};
    static long PlayFabSystem(IntPtr p,long exe){
        for(int i=0;i<PfPinSites.Length;i++)
            if(Hex(Read(p,exe+PfPinSites[i],PfPins[i].Length))!=Hex(PfPins[i]))throw new InvalidOperationException("PlayFab layout pin mismatch at exe+0x"+PfPinSites[i].ToString("x"));
        long root=BitConverter.ToInt64(Read(p,exe+PfGlobal,8),0);if(root==0)return 0;
        long owner=BitConverter.ToInt64(Read(p,root+0x5D0,8),0);if(owner==0)return 0;
        return BitConverter.ToInt64(Read(p,owner+0xA0,8),0);
    }
    // Read-only: PlayFab login state of the engine.
    public static string DiagnosePlayFab(int pid,string expectedExe){
        IntPtr target=OpenProcess(0x0410,false,pid);if(target==IntPtr.Zero)throw Error("OpenProcess");
        try{
            long exe=FindModule(target,expectedExe);if(exe==0)return "exe_not_found";
            long sys=PlayFabSystem(target,exe);if(sys==0)return "system=0";
            long web=BitConverter.ToInt64(Read(target,sys+0x50,8),0);
            string url=web==0?"":Encoding.ASCII.GetString(Read(target,web+0x90,0x80)).Split('\0')[0];
            return String.Format("title={0} loggedIn={1} peer={2:x} url={3}",Encoding.ASCII.GetString(Read(target,sys+0x20,8)).Split('\0')[0],
                Read(target,sys+0x28,1)[0],BitConverter.ToInt64(Read(target,sys+0xB0,8),0),url);
        }catch(Win32Exception e){return "read_failed:"+e.Message;}
        catch(InvalidOperationException e){return "pin_failed:"+e.Message;}
        finally{CloseHandle(target);}
    }
    // DATA-only: replace the web API base URL with the local backend's PlayFab prefix as soon as the engine has formatted it,
    // so its PlayFab requests (login with the genuine Steam ticket, profiles) reach the loopback backend instead of
    // playfabapi.com. Runs on a background thread; poll the returned object.
    public class PlayFabRedirect { public volatile string Outcome="waiting"; public string Before; public long FoundMilliseconds=-1,Samples; }
    public static PlayFabRedirect StartPlayFabRedirect(int pid,string expectedExe,string baseUrl,int deadlineMs){
        if(!Regex.IsMatch(baseUrl??"","^http://127\\.0\\.0\\.1:[0-9]{1,5}/playfab$"))throw new ArgumentException("Only a loopback /playfab prefix is accepted.");
        var state=new PlayFabRedirect();
        var worker=new Thread(()=>{
            IntPtr target=OpenProcess(0x0438,false,pid);
            if(target==IntPtr.Zero){state.Outcome="open_failed";return;}
            bool timer=timeBeginPeriod(1)==0;var clock=Stopwatch.StartNew();
            try{
                var path=new StringBuilder(1024);int length=path.Capacity;
                if(!QueryFullProcessImageName(target,0,path,ref length)||!String.Equals(path.ToString(),expectedExe,StringComparison.OrdinalIgnoreCase)){state.Outcome="unexpected_image";return;}
                long exe=0;string lastPin=null;
                // deadlineMs < 0: for as long as the game runs (a start-up dialog, such as the GPU driver warning, can
                // hold the game for minutes before it formats the URL)
                while((deadlineMs<0||clock.ElapsedMilliseconds<deadlineMs)&&GetExitCodeProcess(target,out uint code)&&code==259){
                    state.Samples++;
                    try{
                        if(exe==0)exe=FindModule(target,expectedExe);
                        long sys=exe==0?0:PlayFabSystem(target,exe);
                        long web=sys==0?0:BitConverter.ToInt64(Read(target,sys+0x50,8),0);
                        if(web!=0){
                            string url=Encoding.ASCII.GetString(Read(target,web+0x90,0x80)).Split('\0')[0];
                            if(url==baseUrl){state.Outcome="already_redirected";return;}
                            if(Regex.IsMatch(url,"^https://[0-9A-F]{4,6}\\.playfabapi\\.com$")){
                                state.Before=url;state.FoundMilliseconds=clock.ElapsedMilliseconds;
                                byte[] value=Encoding.ASCII.GetBytes(baseUrl+"\0");
                                WriteBytes(target,web+0x90,value);
                                state.Outcome=Encoding.ASCII.GetString(Read(target,web+0x90,0x80)).Split('\0')[0]==baseUrl?"redirected":"readback_mismatch";
                                return;
                            }
                        }
                    }catch(Win32Exception){}
                    // Until the protector has decoded the exe the pinned code reads differently: keep waiting, never write.
                    catch(InvalidOperationException e){lastPin=e.Message;}
                    Thread.Sleep(1);
                }
                state.Outcome=lastPin!=null&&state.Before==null?"pin_failed:"+lastPin:"web_api_not_observed";
            }catch(Exception e){state.Outcome="failed:"+e.Message;}
            finally{if(timer)timeEndPeriod(1);CloseHandle(target);}
        });
        worker.IsBackground=true;worker.Start();
        return state;
    }
    public static HellForgeLocalRoutingResult Apply(int pid,long startTicks,string expectedExe,string expectedDll,string url,int deadlineMs,bool applyGate=false){
        if(IntPtr.Size!=8||pid==Process.GetCurrentProcess().Id)throw new InvalidOperationException("External x64 target required.");
        byte[] cave=CaveBytes(url);
        var r=new HellForgeLocalRoutingResult{ProcessId=pid,Url=url,Outcome="not_applied"};
        var clock=Stopwatch.StartNew();var paused=new Stopwatch();var threads=new Dictionary<int,IntPtr>();
        IntPtr target=IntPtr.Zero;Process process=null;bool timer=false;long dll=0;
        try {
            target=OpenProcess(0x0438,false,pid);if(target==IntPtr.Zero)throw Error("OpenProcess");
            var path=new StringBuilder(1024);int length=path.Capacity;
            if(!QueryFullProcessImageName(target,0,path,ref length))throw Error("Process identity");
            if(!String.Equals(path.ToString(),expectedExe,StringComparison.OrdinalIgnoreCase))throw new InvalidOperationException("Unexpected process image.");
            process=Process.GetProcessById(pid);
            if(process.HasExited||process.StartTime.Ticks!=startTicks)throw new InvalidOperationException("Process start identity changed.");
            timer=timeBeginPeriod(1)==0;bool ready=false;
            while(clock.ElapsedMilliseconds<deadlineMs){
                r.Samples++;
                if(process.HasExited){r.Outcome="target_exited_before_patch";return r;}
                try {
                    if(dll==0){dll=FindModule(target,expectedDll);if(dll!=0)r.ModuleSeenMilliseconds=clock.ElapsedMilliseconds;}
                    if(dll!=0){
                        string site=Hex(Read(target,dll+Site,SiteBefore.Length));
                        if(site==Hex(SiteAfter)){r.Outcome="already_patched";return r;}
                        // Until WinLicense finishes decoding, the site holds other bytes; keep waiting.
                        if(site==Hex(SiteBefore)&&IsPadding(Read(target,dll+Cave,CaveSize))){ready=true;break;}
                    }
                }catch(Win32Exception){r.ReadFailures++;}
                Thread.Sleep(1);
            }
            r.DllBase="0x"+dll.ToString("x");
            if(!ready){r.Outcome=dll==0?"module_not_observed":"decoded_bytes_not_observed";return r;}
            paused.Start();bool stable=false;
            for(int pass=0;pass<8&&paused.ElapsedMilliseconds<1500;pass++){
                int added=0;process.Refresh();
                foreach(ProcessThread t in process.Threads){
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
                        r.Threads.Add(new HellForgeLocalRoutingThread{Id=t.Id,Rip="0x"+rip.ToString("x")});
                        if(rip>=dll+Site&&rip<dll+Site+SiteBefore.Length)throw new InvalidOperationException("Thread executing routing site.");
                    }finally{Marshal.FreeHGlobal(raw);}
                }
                if(added==0&&threads.Count>0){stable=true;break;}
            }
            if(!stable)throw new TimeoutException("Stable thread-set deadline exceeded.");
            r.SiteBefore=Hex(Read(target,dll+Site,SiteBefore.Length));
            if(r.SiteBefore!=Hex(SiteBefore)||!IsPadding(Read(target,dll+Cave,CaveSize))){r.Outcome="decoded_bytes_changed";return r;}
            r.WriteAttempted=true;
            // Cave first: the site never references unwritten bytes.
            WriteBytes(target,dll+Cave,cave);r.CaveWritten=true;
            WriteBytes(target,dll+Site,SiteAfter);r.SiteWritten=true;
            r.CaveAfter=Encoding.ASCII.GetString(Read(target,dll+Cave,cave.Length-1));
            r.SiteAfter=Hex(Read(target,dll+Site,SiteAfter.Length));
            if(r.SiteAfter!=Hex(SiteAfter)||r.CaveAfter!=url)throw new InvalidOperationException("Readback mismatch.");
            r.ReadbackMatched=true;
            // Gate flips are opt-in: the first boot test showed editing this WinLicense-protected
            // code region made the game exit early (likely anti-tamper). Default leaves it unpatched.
            if(applyGate){
            // Verify every original before writing any.
            for(int i=0;i<GateSites.Length;i++)
                if(Hex(Read(target,dll+GateSites[i],GateBefore[i].Length))!=Hex(GateBefore[i]))throw new InvalidOperationException("Gate branch not at expected bytes.");
            for(int i=0;i<GateSites.Length;i++){
                WriteBytes(target,dll+GateSites[i],GateAfter[i]);r.GateBranchesWritten++;
                if(Hex(Read(target,dll+GateSites[i],GateAfter[i].Length))!=Hex(GateAfter[i]))throw new InvalidOperationException("Gate readback mismatch.");
            }
            r.GateReadbackMatched=true;
            }
            r.Outcome="applied";
        }catch(Exception e){
            r.Failure=e.ToString();r.Outcome="patch_failed";
            if(r.WriteAttempted)try{
                for(int i=0;i<r.GateBranchesWritten;i++)WriteBytes(target,dll+GateSites[i],GateBefore[i]);
                if(r.SiteWritten)WriteBytes(target,dll+Site,SiteBefore);
                if(r.CaveWritten){var pad=new byte[cave.Length];for(int i=0;i<pad.Length;i++)pad[i]=0xcc;WriteBytes(target,dll+Cave,pad);}
                r.RolledBack=true;
            }catch{r.Terminated=TerminateProcess(target,0xb7520001);}
        }finally{
            foreach(IntPtr h in threads.Values){if(ResumeThread(h)==uint.MaxValue){r.Failure+="; resume failed";r.Outcome="patch_failed";r.Terminated=TerminateProcess(target,0xb7520002);}CloseHandle(h);}
            paused.Stop();r.PauseMilliseconds=paused.ElapsedMilliseconds;
            if(timer)timeEndPeriod(1);if(process!=null)process.Dispose();if(target!=IntPtr.Zero)CloseHandle(target);
            r.ElapsedMilliseconds=clock.ElapsedMilliseconds;
        }
        return r;
    }
}
