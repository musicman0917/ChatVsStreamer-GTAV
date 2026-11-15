// A read-only test plugin for RAGE Plugin Hook.
//
// The real plugin will be a thin C# layer that forwards native calls from the C++ core: raw
// 64-bit argument values, called from a C# callback with C++ frames on the stack. Nothing in
// RPH's docs says whether that works, so this checks it on the real game before that layer
// gets written. Each forwarded call is compared with the same value from RPH's own API.
//
// It only reads game state: no spawning, no teleporting, no changes to the player or world.
// Results go to RagePluginHook.log and to %LOCALAPPDATA%\ChatVsStreamerGTAV\rph_probe.log.
// The plugin then ends by itself.

using System;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using Rage;
using Rage.Attributes;
using Rage.Native;

[assembly: Plugin("CvsProbe", Description = "Read-only checks for Chat vs Streamer", Author = "Chat vs Streamer", PrefersSingleInstance = true)]
[assembly: System.Runtime.Versioning.TargetFramework(".NETFramework,Version=v4.8")]

namespace CvsProbe
{
    public static class EntryPoint
    {
        // All checked against both native reference files (Legacy and Enhanced).
        const ulong PLAYER_PED_ID = 0xD80958FC74E988A6;
        const ulong GET_ENTITY_COORDS = 0x3FEF770D40960D5A;
        const ulong GET_ENTITY_HEADING = 0xE83D4F9BA2A38914;
        const ulong GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS = 0x1899F328B0E12848;
        const ulong GET_GROUND_Z_FOR_3D_COORD = 0xC906A7DAB05C8D2B;
        const ulong GET_HASH_KEY = 0xD24D37CC275948CC;
        const ulong GET_SAFE_COORD_FOR_PED = 0xB61C8E878A4199CA;

        static string _logPath;
        static int _passed;
        static int _failed;

        struct Result
        {
            public readonly bool Ok;
            public readonly string Detail;
            public Result(bool ok, string detail) { Ok = ok; Detail = detail; }
        }

        delegate bool EnumWindowsProc(IntPtr hwnd, IntPtr lParam);

        [DllImport("user32.dll")]
        static extern bool EnumWindows(EnumWindowsProc callback, IntPtr lParam);

        public static void Main()
        {
            try
            {
                OpenLog();
                Log("--- CvsProbe started ---");
                LogEnvironment();
                WaitForPlayer();
                Log("Player is ready, running checks");
                RunChecks();
                string summary = "CvsProbe: " + _passed + " passed, " + _failed + " failed";
                Log("DONE " + summary);
                Game.DisplayNotification(summary);
            }
            catch (Exception e)
            {
                Log("FAIL the probe stopped early: " + e);
            }
        }

        static void OpenLog()
        {
            try
            {
                string dir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "ChatVsStreamerGTAV");
                Directory.CreateDirectory(dir);
                _logPath = Path.Combine(dir, "rph_probe.log");
            }
            catch
            {
                _logPath = null;
            }
        }

        static void Log(string text)
        {
            try { Game.LogTrivial("[CVS-PROBE] " + text); } catch { }
            if (_logPath == null) return;
            try { File.AppendAllText(_logPath, DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss ") + text + Environment.NewLine); } catch { }
        }

        static void LogEnvironment()
        {
            Info("Game version", () => Game.ProductVersion + ", build " + Game.BuildNumber);
            Info("Process", () => Process.GetCurrentProcess().MainModule.FileName + ", " + (IntPtr.Size * 8) + "-bit");
            Info("Working directory", () => Environment.CurrentDirectory);
            Info("App domain", () => AppDomain.CurrentDomain.FriendlyName + ", base " + AppDomain.CurrentDomain.BaseDirectory);
            Info("Plugin file", () => "'" + Assembly.GetExecutingAssembly().Location + "'");
            Info("CLR", () => Environment.Version.ToString());
        }

        static void Info(string name, Func<string> value)
        {
            try { Log("INFO " + name + ": " + value()); }
            catch (Exception e) { Log("INFO " + name + ": unavailable (" + e.GetType().Name + ")"); }
        }

        static void WaitForPlayer()
        {
            while (true)
            {
                try
                {
                    if (!Game.IsLoading && Game.IsScreenFadedIn && Game.LocalPlayer.Character.IsValid()) break;
                }
                catch
                {
                    // The player object isn't there yet during the first load.
                }
                GameFiber.Sleep(500);
            }
            // Let the world around the player stream in, so the ground checks have something to find.
            GameFiber.Sleep(5000);
        }

        static void Check(string name, Func<Result> body)
        {
            try
            {
                Result r = body();
                Log((r.Ok ? "PASS " : "FAIL ") + name + ": " + r.Detail);
                if (r.Ok) _passed++; else _failed++;
            }
            catch (Exception e)
            {
                Log("FAIL " + name + ": threw " + e.GetType().Name + ": " + e.Message);
                _failed++;
            }
        }

        // The values the C++ core will send: every argument as one raw 64-bit slot.
        static NativeArgument Raw(uint value) { return new NativeArgument((ulong)value); }
        static NativeArgument RawFloat(float value) { return new NativeArgument((ulong)BitConverter.ToUInt32(BitConverter.GetBytes(value), 0)); }
        static NativeArgument RawPointer(IntPtr value) { return new NativeArgument((ulong)value.ToInt64()); }

        static bool Near(Vector3 a, Vector3 b, float tolerance)
        {
            float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
            return dx * dx + dy * dy + dz * dz <= tolerance * tolerance;
        }

        static string Show(Vector3 v) { return v.X.ToString("F2") + ", " + v.Y.ToString("F2") + ", " + v.Z.ToString("F2"); }

        static void RunChecks()
        {
            Ped ped = Game.LocalPlayer.Character;
            uint handle = ped.Handle;
            Vector3 position = ped.Position;
            float heading = ped.Heading;
            Vector3 ahead = ped.GetOffsetPosition(new Vector3(0f, 5f, 0f));

            Check("int return, no arguments", () =>
            {
                int got = NativeFunction.CallByHash<int>(PLAYER_PED_ID);
                return new Result((uint)got == handle, "got " + got + ", RPH says " + handle);
            });

            Check("Vector3 return, raw int arguments", () =>
            {
                Vector3 got = NativeFunction.CallByHash<Vector3>(GET_ENTITY_COORDS, Raw(handle), Raw(1));
                return new Result(Near(got, position, 1f), "got " + Show(got) + ", RPH says " + Show(position));
            });

            Check("float return, typed", () =>
            {
                float got = NativeFunction.CallByHash<float>(GET_ENTITY_HEADING, Raw(handle));
                return new Result(Math.Abs(got - heading) < 1f, "got " + got + ", RPH says " + heading);
            });

            Check("float return, read as a raw value", () =>
            {
                ulong raw = NativeFunction.CallByHash<ulong>(GET_ENTITY_HEADING, Raw(handle));
                float got = BitConverter.ToSingle(BitConverter.GetBytes((uint)raw), 0);
                return new Result(Math.Abs(got - heading) < 1f, "got " + got + " from 0x" + raw.ToString("X16") + ", RPH says " + heading);
            });

            Check("float arguments, typed", () =>
            {
                Vector3 got = NativeFunction.CallByHash<Vector3>(GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS, Raw(handle), new NativeArgument(0f), new NativeArgument(5f), new NativeArgument(0f));
                return new Result(Near(got, ahead, 1f), "got " + Show(got) + ", RPH says " + Show(ahead));
            });

            Check("float arguments, raw bits", () =>
            {
                Vector3 got = NativeFunction.CallByHash<Vector3>(GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS, Raw(handle), RawFloat(0f), RawFloat(5f), RawFloat(0f));
                return new Result(Near(got, ahead, 1f), "got " + Show(got) + ", RPH says " + Show(ahead));
            });

            Check("float output through a raw pointer", () =>
            {
                IntPtr buffer = Marshal.AllocHGlobal(16);
                try
                {
                    Marshal.WriteInt64(buffer, 0, 0);
                    Marshal.WriteInt64(buffer, 8, 0);
                    int found = NativeFunction.CallByHash<int>(GET_GROUND_Z_FOR_3D_COORD, RawFloat(position.X), RawFloat(position.Y), RawFloat(position.Z + 1f), RawPointer(buffer), Raw(0), Raw(0));
                    float ground = ReadFloat(buffer, 0);
                    return new Result((found & 0xFF) != 0 && Math.Abs(ground - position.Z) < 25f, "returned " + found + ", ground " + ground + ", player height " + position.Z);
                }
                finally { Marshal.FreeHGlobal(buffer); }
            });

            Check("Vector3 output through a raw pointer", () =>
            {
                IntPtr buffer = Marshal.AllocHGlobal(32);
                try
                {
                    for (int i = 0; i < 32; i += 8) Marshal.WriteInt64(buffer, i, 0);
                    int found = NativeFunction.CallByHash<int>(GET_SAFE_COORD_FOR_PED, RawFloat(position.X), RawFloat(position.Y), RawFloat(position.Z), Raw(0), RawPointer(buffer), Raw(0));
                    Vector3 got = new Vector3(ReadFloat(buffer, 0), ReadFloat(buffer, 8), ReadFloat(buffer, 16));
                    if ((found & 0xFF) == 0) return new Result(true, "no safe spot here, so this proves little. Output " + Show(got));
                    return new Result(Near(got, position, 300f), "got " + Show(got) + ", player at " + Show(position));
                }
                finally { Marshal.FreeHGlobal(buffer); }
            });

            Check("string through a raw pointer", () =>
            {
                IntPtr text = Marshal.StringToHGlobalAnsi("adder");
                try
                {
                    uint got = NativeFunction.CallByHash<uint>(GET_HASH_KEY, RawPointer(text));
                    uint expected = Game.GetHashKey("adder");
                    return new Result(got == expected, "got " + got + ", RPH says " + expected);
                }
                finally { Marshal.FreeHGlobal(text); }
            });

            // The shape the real plugin depends on: C# calls unmanaged code, which calls back into
            // C#, which calls a native, all on one game fiber.
            Check("native call with unmanaged frames on the stack", () =>
            {
                Vector3 got = default(Vector3);
                int calls = 0;
                Exception failure = null;
                EnumWindowsProc callback = (hwnd, lParam) =>
                {
                    calls++;
                    try { got = NativeFunction.CallByHash<Vector3>(GET_ENTITY_COORDS, Raw(handle), Raw(1)); }
                    catch (Exception e) { failure = e; }
                    return false;
                };
                EnumWindows(callback, IntPtr.Zero);
                GC.KeepAlive(callback);
                if (failure != null) return new Result(false, "threw " + failure.GetType().Name + ": " + failure.Message);
                return new Result(calls == 1 && Near(got, position, 1f), "callback ran " + calls + " time(s), got " + Show(got));
            });

            Check("the Natives object, as a fallback", () =>
            {
                dynamic natives = NativeFunction.Natives;
                int got = natives.PLAYER_PED_ID<int>();
                return new Result((uint)got == handle, "got " + got + ", RPH says " + handle);
            });

            Check("vehicle list", () =>
            {
                Vehicle[] vehicles = World.GetAllVehicles();
                string first = vehicles.Length > 0 ? ", first handle " + (uint)vehicles[0].Handle : "";
                return new Result(vehicles != null, vehicles.Length + " vehicles" + first);
            });

            Check("cost of a forwarded call", () =>
            {
                const int count = 200;
                Stopwatch timer = Stopwatch.StartNew();
                for (int i = 0; i < count; i++) NativeFunction.CallByHash<Vector3>(GET_ENTITY_COORDS, Raw(handle), Raw(1));
                timer.Stop();
                return new Result(true, (timer.Elapsed.TotalMilliseconds * 1000.0 / count).ToString("F1") + " microseconds per call");
            });

            Check("ticks per second", () =>
            {
                int ticks = 0;
                Stopwatch timer = Stopwatch.StartNew();
                while (timer.ElapsedMilliseconds < 3000)
                {
                    ticks++;
                    GameFiber.Yield();
                }
                double rate = ticks / timer.Elapsed.TotalSeconds;
                return new Result(rate >= 5.0, rate.ToString("F0") + " ticks per second, game reports " + Game.FrameRate.ToString("F0") + " frames per second");
            });
        }

        static float ReadFloat(IntPtr buffer, int offset)
        {
            return BitConverter.ToSingle(BitConverter.GetBytes(Marshal.ReadInt32(buffer, offset)), 0);
        }
    }
}
