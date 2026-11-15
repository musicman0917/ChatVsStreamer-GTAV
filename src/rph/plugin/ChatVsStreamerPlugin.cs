// The thin layer that runs the C++ core under RAGE Plugin Hook. RPH only loads .NET plugins,
// so this loads ChatVsStreamerCore.dll, ticks it once per frame on a game fiber, and forwards
// its native calls to the game. Chat, points, effects and the web server are all in the core.
//
// tools/rph_probe checks each kind of call used here against the real game.

using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using Rage;
using Rage.Attributes;
using Rage.Native;

[assembly: Plugin("Chat vs Streamer", Description = "Lets Twitch viewers spend points to sabotage or bless you in story mode", Author = "NeighborhoodofMusic", PrefersSingleInstance = true, ShouldTickInPauseMenu = true)]
[assembly: System.Runtime.Versioning.TargetFramework(".NETFramework,Version=v4.8")]

namespace ChatVsStreamer
{
    public static unsafe class EntryPoint
    {
        // Has to match kCvsRphApiVersion in src/rph/RphApi.h.
        const int ApiVersion = 2;

        // RPH loads this plugin from a temp copy, so the path can't be relative to this file.
        // The app domain's base directory is the game folder.
        const string CorePath = @"Plugins\ChatVsStreamer\ChatVsStreamerCore.dll";

        // Has to match CvsRphHost in src/rph/RphApi.h.
        [StructLayout(LayoutKind.Sequential)]
        struct Host
        {
            public IntPtr CallInt;
            public IntPtr CallFloat;
            public IntPtr CallVector;
            public IntPtr GetAllVehicles;
            public IntPtr GetAllPeds;
            public IntPtr IsKeyDown;
        }

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int CallIntFn(ulong hash, ulong* args, int count);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate float CallFloatFn(ulong hash, ulong* args, int count);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate void CallVectorFn(ulong hash, ulong* args, int count, float* xyz);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int GetAllEntitiesFn(int* handles, int capacity);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int StartFn(int apiVersion, ref Host host, [MarshalAs(UnmanagedType.LPStr)] string gameVersion);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate void TickFn();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate void StopFn();

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern IntPtr LoadLibraryW(string path);

        [DllImport("kernel32.dll", CharSet = CharSet.Ansi, ExactSpelling = true)]
        static extern IntPtr GetProcAddress(IntPtr module, string name);

        // Held in fields because the core keeps pointers to them, which the garbage collector can't see.
        static readonly CallIntFn s_callInt = CallInt;
        static readonly CallFloatFn s_callFloat = CallFloat;
        static readonly CallVectorFn s_callVector = CallVector;
        static readonly GetAllEntitiesFn s_getAllVehicles = GetAllVehicles;
        static readonly GetAllEntitiesFn s_getAllPeds = GetAllPeds;

        static readonly HashSet<ulong> s_reportedNatives = new HashSet<ulong>();

        static StartFn s_start;
        static TickFn s_tick;
        static StopFn s_stop;

        public static void Main()
        {
            try
            {
                if (!LoadCore()) return;

                Host host = new Host
                {
                    CallInt = Marshal.GetFunctionPointerForDelegate(s_callInt),
                    CallFloat = Marshal.GetFunctionPointerForDelegate(s_callFloat),
                    CallVector = Marshal.GetFunctionPointerForDelegate(s_callVector),
                    GetAllVehicles = Marshal.GetFunctionPointerForDelegate(s_getAllVehicles),
                    GetAllPeds = Marshal.GetFunctionPointerForDelegate(s_getAllPeds),
                    IsKeyDown = IntPtr.Zero,
                };
                int result = s_start(ApiVersion, ref host, Game.ProductVersion.ToString());
                if (result != 0)
                {
                    Fail("ChatVsStreamer.dll and ChatVsStreamerCore.dll come from different builds (error " + result + "). Reinstall the mod.");
                    return;
                }
                Log("started");
            }
            catch (Exception e)
            {
                Fail("couldn't start: " + e);
                return;
            }

            while (true)
            {
                s_tick();
                GameFiber.Yield();
            }
        }

        public static void OnUnload(bool isTerminating)
        {
            // Must not sleep here if isTerminating is set, and this doesn't.
            if (s_stop != null) s_stop();
        }

        static bool LoadCore()
        {
            string path = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, CorePath);
            IntPtr module = LoadLibraryW(path);
            if (module == IntPtr.Zero)
            {
                int error = Marshal.GetLastWin32Error();
                const int BlockedByAppControl = 4551; // usually Smart App Control refusing an unsigned file
                Fail("couldn't load " + path + " (Windows error " + error + (error == BlockedByAppControl ? ", which means Smart App Control or a similar policy blocked it" : "") + ")");
                return false;
            }
            s_start = Bind<StartFn>(module, "cvs_rph_start");
            s_tick = Bind<TickFn>(module, "cvs_rph_tick");
            s_stop = Bind<StopFn>(module, "cvs_rph_stop");
            return true;
        }

        static T Bind<T>(IntPtr module, string name)
        {
            IntPtr address = GetProcAddress(module, name);
            if (address == IntPtr.Zero) throw new EntryPointNotFoundException(name + " isn't in " + CorePath);
            return Marshal.GetDelegateForFunctionPointer<T>(address);
        }

        // ── Called by the core, with C++ frames underneath ───────────────────────────
        // An exception that reached those frames would skip their cleanup, so nothing gets past these.

        static NativeArgument[] Arguments(ulong* args, int count)
        {
            NativeArgument[] arguments = new NativeArgument[count];
            for (int i = 0; i < count; i++) arguments[i] = new NativeArgument(args[i]);
            return arguments;
        }

        static int CallInt(ulong hash, ulong* args, int count)
        {
            try { return NativeFunction.CallByHash<int>(hash, Arguments(args, count)); }
            catch (Exception e) { ReportNative(hash, e); return 0; }
        }

        static float CallFloat(ulong hash, ulong* args, int count)
        {
            try { return NativeFunction.CallByHash<float>(hash, Arguments(args, count)); }
            catch (Exception e) { ReportNative(hash, e); return 0f; }
        }

        static void CallVector(ulong hash, ulong* args, int count, float* xyz)
        {
            try
            {
                Vector3 v = NativeFunction.CallByHash<Vector3>(hash, Arguments(args, count));
                xyz[0] = v.X;
                xyz[1] = v.Y;
                xyz[2] = v.Z;
            }
            catch (Exception e) { ReportNative(hash, e); }
        }

        static int GetAllVehicles(int* handles, int capacity)
        {
            try
            {
                Vehicle[] vehicles = World.GetAllVehicles();
                int count = Math.Min(vehicles.Length, capacity);
                for (int i = 0; i < count; i++) handles[i] = (int)(uint)vehicles[i].Handle;
                return count;
            }
            catch (Exception e)
            {
                Log("listing vehicles failed: " + e.GetType().Name + ": " + e.Message);
                return 0;
            }
        }

        static int GetAllPeds(int* handles, int capacity)
        {
            try
            {
                Ped[] peds = World.GetAllPeds();
                int count = Math.Min(peds.Length, capacity);
                for (int i = 0; i < count; i++) handles[i] = (int)(uint)peds[i].Handle;
                return count;
            }
            catch (Exception e)
            {
                Log("listing peds failed: " + e.GetType().Name + ": " + e.Message);
                return 0;
            }
        }

        // ── Reporting ───────────────────────────────────────────────────────────────

        // Once per native, because a failing one could otherwise write to the log every frame.
        static void ReportNative(ulong hash, Exception e)
        {
            if (s_reportedNatives.Add(hash)) Log("native 0x" + hash.ToString("X16") + " failed: " + e.GetType().Name + ": " + e.Message);
        }

        // RPH starts each line with the plugin's name.
        static void Log(string text)
        {
            try { Game.LogTrivial(text); } catch { }
        }

        static void Fail(string text)
        {
            Log(text);
            try { Game.DisplayNotification("Chat vs Streamer " + (text.Length > 80 ? "couldn't start. See RagePluginHook.log" : text)); } catch { }
        }
    }
}
