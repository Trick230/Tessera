// SPDX-License-Identifier: GPL-3.0-or-later
// Records the player's keyboard and mouse (raw input) during one run of the
// global test and replays them in the next runs (SendInput), so every run
// plays the same route. Loaded by tools/global_test.ps1 through Add-Type
// (C# 5: Windows PowerShell's compiler).
//
// The timeline starts when the game's process appears. The mark key (F10)
// splits it: before the mark (menus, loading the save) only keys are kept and
// replayed with longer gaps, since loading times vary and a menu waits for
// input anyway; after the mark (the route) keys and mouse are replayed with
// the recorded timing.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows.Forms;

namespace TesseraInput
{
    // Kind 0 = key: Code = scan code, Flags = RI_KEY_* (1 break, 2 E0, 4 E1).
    // Kind 1 = mouse: Dx/Dy relative motion, Flags = RI_MOUSE_* buttons, Data = wheel.
    public struct Ev
    {
        public long Us;
        public int Kind, Code, Flags, Dx, Dy, Data;
    }

    public class Route
    {
        public List<Ev> Events = new List<Ev>();
        public long MarkUs = -1;

        public void Save(string path)
        {
            using (StreamWriter w = new StreamWriter(path))
            {
                w.WriteLine("tessera-route 1 mark " + MarkUs);
                foreach (Ev e in Events)
                    w.WriteLine(string.Join(" ", new string[] { e.Us.ToString(), e.Kind.ToString(), e.Code.ToString(),
                        e.Flags.ToString(), e.Dx.ToString(), e.Dy.ToString(), e.Data.ToString() }));
            }
        }

        public static Route Load(string path)
        {
            Route r = new Route();
            string[] lines = File.ReadAllLines(path);
            string[] h = lines[0].Split(' ');
            if (h.Length < 4 || h[0] != "tessera-route") throw new InvalidDataException("not a route file: " + path);
            r.MarkUs = long.Parse(h[3], CultureInfo.InvariantCulture);
            for (int i = 1; i < lines.Length; i++)
            {
                string[] f = lines[i].Split(' ');
                if (f.Length < 7) continue;
                Ev e = new Ev();
                e.Us = long.Parse(f[0]); e.Kind = int.Parse(f[1]); e.Code = int.Parse(f[2]); e.Flags = int.Parse(f[3]);
                e.Dx = int.Parse(f[4]); e.Dy = int.Parse(f[5]); e.Data = int.Parse(f[6]);
                r.Events.Add(e);
            }
            return r;
        }

        // Seconds of route after the mark.
        public double RouteSeconds()
        {
            return Events.Count == 0 ? 0 : (Events[Events.Count - 1].Us - MarkUs) / 1e6;
        }
    }

    public class Recorder : NativeWindow
    {
        [StructLayout(LayoutKind.Sequential)]
        struct RAWINPUTDEVICE { public ushort Page, Usage; public uint Flags; public IntPtr Target; }
        [DllImport("user32.dll")] static extern bool RegisterRawInputDevices(RAWINPUTDEVICE[] d, uint n, uint size);
        [DllImport("user32.dll")] static extern uint GetRawInputData(IntPtr h, uint cmd, IntPtr data, ref uint size, uint header);
        [DllImport("user32.dll")] static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);

        const int WM_INPUT = 0x00FF, WM_STOP = 0x8001;
        static Recorder win;
        static Thread thread;
        static Stopwatch clock;
        static Route route;
        static int markVk;
        static readonly HashSet<int> down = new HashSet<int>();
        static readonly IntPtr buf = Marshal.AllocHGlobal(512);
        public static ManualResetEvent Marked = new ManualResetEvent(false);
        public static string Error;

        // Starts recording on its own thread (raw input needs a window and a message loop).
        public static void Start(int markKey)
        {
            route = new Route();
            markVk = markKey;
            down.Clear();
            Marked.Reset();
            Error = null;
            ManualResetEvent ready = new ManualResetEvent(false);
            thread = new Thread(delegate ()
            {
                win = new Recorder();
                win.CreateHandle(new CreateParams());
                RAWINPUTDEVICE[] d = new RAWINPUTDEVICE[2];
                d[0].Page = 1; d[0].Usage = 6; d[0].Flags = 0x100; d[0].Target = win.Handle;  // keyboard, RIDEV_INPUTSINK
                d[1].Page = 1; d[1].Usage = 2; d[1].Flags = 0x100; d[1].Target = win.Handle;  // mouse
                if (!RegisterRawInputDevices(d, 2, (uint)Marshal.SizeOf(typeof(RAWINPUTDEVICE))))
                    Error = "RegisterRawInputDevices failed: " + Marshal.GetLastWin32Error();
                clock = Stopwatch.StartNew();
                ready.Set();
                Application.Run();
                win.DestroyHandle();
            });
            thread.IsBackground = true;
            thread.SetApartmentState(ApartmentState.STA);
            thread.Start();
            ready.WaitOne();
        }

        public static Route Stop()
        {
            PostMessage(win.Handle, WM_STOP, IntPtr.Zero, IntPtr.Zero);
            thread.Join();
            lock (route) return route;
        }

        protected override void WndProc(ref Message m)
        {
            if (m.Msg == WM_INPUT) Read(m.LParam);
            else if (m.Msg == WM_STOP) { Application.ExitThread(); return; }
            base.WndProc(ref m);
        }

        static void Read(IntPtr h)
        {
            uint hs = (uint)(IntPtr.Size == 8 ? 24 : 16);
            uint size = 512;
            if (GetRawInputData(h, 0x10000003, buf, ref size, hs) == uint.MaxValue) return;
            int o = (int)hs;
            Ev e = new Ev();
            e.Us = clock.ElapsedTicks * 1000000 / Stopwatch.Frequency;
            int type = Marshal.ReadInt32(buf, 0);
            if (type == 1)
            {
                e.Kind = 0;
                e.Code = (ushort)Marshal.ReadInt16(buf, o);
                e.Flags = (ushort)Marshal.ReadInt16(buf, o + 2);
                int vk = (ushort)Marshal.ReadInt16(buf, o + 6);
                bool up = (e.Flags & 1) != 0;
                if (vk == markVk)
                {
                    if (!up && route.MarkUs < 0) { route.MarkUs = e.Us; Marked.Set(); }
                    return;
                }
                // Autorepeat: keep the first make of a held key only.
                int key = e.Code | (e.Flags & 6) << 16;
                if (up) down.Remove(key);
                else if (!down.Add(key)) return;
            }
            else if (type == 0)
            {
                if ((Marshal.ReadInt16(buf, o) & 1) != 0) return;  // absolute (tablet, remote desktop)
                e.Kind = 1;
                e.Flags = (ushort)Marshal.ReadInt16(buf, o + 4);
                e.Data = Marshal.ReadInt16(buf, o + 6);
                e.Dx = Marshal.ReadInt32(buf, o + 12);
                e.Dy = Marshal.ReadInt32(buf, o + 16);
                if (e.Dx == 0 && e.Dy == 0 && e.Flags == 0) return;
            }
            else return;
            lock (route) route.Events.Add(e);
        }
    }

    public static class Player
    {
        [StructLayout(LayoutKind.Sequential)]
        struct MOUSEINPUT { public int dx, dy; public uint data, flags, time; public IntPtr extra; }
        [StructLayout(LayoutKind.Sequential)]
        struct KEYBDINPUT { public ushort vk, scan; public uint flags, time; public IntPtr extra; }
        [StructLayout(LayoutKind.Explicit)]
        struct UNION { [FieldOffset(0)] public MOUSEINPUT mi; [FieldOffset(0)] public KEYBDINPUT ki; }
        [StructLayout(LayoutKind.Sequential)]
        struct INPUT { public uint type; public UNION u; }
        [DllImport("user32.dll")] static extern uint SendInput(uint n, INPUT[] inputs, int size);
        [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
        [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr h);
        [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
        [DllImport("winmm.dll")] static extern uint timeBeginPeriod(uint ms);
        [DllImport("winmm.dll")] static extern uint timeEndPeriod(uint ms);

        static int sent, unfocused;
        static readonly HashSet<int> keysDown = new HashSet<int>();
        static int buttonsDown;

        // Before the mark: keys only, each press after its recorded gap x1.5 + 1 s,
        // then the gap to the mark x1.5 + 15 s (the save loads meanwhile).
        public static string Lead(Route r, Process p)
        {
            Begin();
            Stopwatch clock = Stopwatch.StartNew();
            long prev = 0, target = 0;
            foreach (Ev e in r.Events)
            {
                if (e.Us >= r.MarkUs) break;
                if (e.Kind != 0) continue;
                long gap = e.Us - prev;
                target += (e.Flags & 1) == 0 ? gap * 3 / 2 + 1000000 : gap;
                prev = e.Us;
                if (!WaitUntil(clock, target, p)) return End("the game exited during the lead");
                Send(e, p);
            }
            target += (r.MarkUs - prev) * 3 / 2 + 15000000;
            bool alive = WaitUntil(clock, target, p);
            ReleaseAll();
            return End(alive ? "lead done" : "the game exited during the lead");
        }

        // After the mark: keys and mouse with the recorded timing, for up to seconds.
        public static string Play(Route r, Process p, double seconds)
        {
            Begin();
            Stopwatch clock = Stopwatch.StartNew();
            long end = (long)(seconds * 1e6);
            string why = "route done";
            foreach (Ev e in r.Events)
            {
                long t = e.Us - r.MarkUs;
                if (t <= 0) continue;
                if (t > end) break;
                if (!WaitUntil(clock, t, p)) { why = "the game exited during the route"; break; }
                Send(e, p);
            }
            ReleaseAll();
            return End(why);
        }

        static void Begin()
        {
            sent = 0; unfocused = 0; keysDown.Clear(); buttonsDown = 0;
            timeBeginPeriod(1);
        }

        static string End(string why)
        {
            timeEndPeriod(1);
            return why + ": " + sent + " inputs sent, " + unfocused + " while the game was not in front";
        }

        static bool WaitUntil(Stopwatch clock, long us, Process p)
        {
            for (;;)
            {
                long left = us - clock.ElapsedTicks * 1000000 / Stopwatch.Frequency;
                if (left <= 0) return true;
                if (p.HasExited) return false;
                if (left > 2000) Thread.Sleep(1); else Thread.SpinWait(50);
            }
        }

        static bool Focused(Process p)
        {
            uint pid;
            GetWindowThreadProcessId(GetForegroundWindow(), out pid);
            if (pid == p.Id) return true;
            p.Refresh();
            if (p.MainWindowHandle != IntPtr.Zero) SetForegroundWindow(p.MainWindowHandle);
            return false;
        }

        static void Send(Ev e, Process p)
        {
            if (!Focused(p)) unfocused++;
            List<INPUT> list = new List<INPUT>(4);
            if (e.Kind == 0)
            {
                if ((e.Flags & 4) != 0) return;  // E1 (Pause): not worth replaying
                bool up = (e.Flags & 1) != 0;
                int key = e.Code | (e.Flags & 6) << 16;
                if (up) keysDown.Remove(key); else keysDown.Add(key);
                list.Add(Key(e.Code, (e.Flags & 2) != 0, up));
            }
            else
            {
                if (e.Dx != 0 || e.Dy != 0) list.Add(Mouse(e.Dx, e.Dy, 1, 0));
                // RI_MOUSE_* -> MOUSEEVENTF_*: left, right, middle down/up.
                int[] ri = { 0x1, 0x2, 0x4, 0x8, 0x10, 0x20 };
                uint[] mf = { 0x2, 0x4, 0x8, 0x10, 0x20, 0x40 };
                for (int i = 0; i < ri.Length; i++)
                    if ((e.Flags & ri[i]) != 0)
                    {
                        list.Add(Mouse(0, 0, mf[i], 0));
                        if ((i & 1) == 0) buttonsDown |= 1 << (i / 2); else buttonsDown &= ~(1 << (i / 2));
                    }
                if ((e.Flags & 0x40) != 0) list.Add(Mouse(0, 0, 0x80, 1));   // X1 down
                if ((e.Flags & 0x80) != 0) list.Add(Mouse(0, 0, 0x100, 1));  // X1 up
                if ((e.Flags & 0x100) != 0) list.Add(Mouse(0, 0, 0x80, 2));  // X2 down
                if ((e.Flags & 0x200) != 0) list.Add(Mouse(0, 0, 0x100, 2)); // X2 up
                if ((e.Flags & 0x400) != 0) list.Add(Mouse(0, 0, 0x800, (uint)e.Data));  // wheel
            }
            if (list.Count == 0) return;
            sent += (int)SendInput((uint)list.Count, list.ToArray(), Marshal.SizeOf(typeof(INPUT)));
        }

        // Nothing stays held after a replay.
        static void ReleaseAll()
        {
            List<INPUT> list = new List<INPUT>();
            foreach (int key in keysDown) list.Add(Key(key & 0xFFFF, (key >> 16 & 2) != 0, true));
            uint[] ups = { 0x4, 0x10, 0x40 };
            for (int i = 0; i < 3; i++) if ((buttonsDown & 1 << i) != 0) list.Add(Mouse(0, 0, ups[i], 0));
            if (list.Count > 0) SendInput((uint)list.Count, list.ToArray(), Marshal.SizeOf(typeof(INPUT)));
            keysDown.Clear();
            buttonsDown = 0;
        }

        static INPUT Key(int scan, bool e0, bool up)
        {
            INPUT i = new INPUT();
            i.type = 1;
            i.u.ki.scan = (ushort)scan;
            i.u.ki.flags = 0x8 | (e0 ? 0x1u : 0) | (up ? 0x2u : 0);  // SCANCODE | EXTENDEDKEY | KEYUP
            return i;
        }

        static INPUT Mouse(int dx, int dy, uint flags, uint data)
        {
            INPUT i = new INPUT();
            i.type = 0;
            i.u.mi.dx = dx; i.u.mi.dy = dy; i.u.mi.flags = flags; i.u.mi.data = data;
            return i;
        }
    }
}
