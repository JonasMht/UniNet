// UniNet cross-language interop: the C# participant.
//
// See interop_cpp.cpp for what this proves. On top of that, this participant
// checks the one thing only C# has: that a session joined with
// marshalToCaller: false runs SubscribeCbor handlers off the thread that joined
// (so a Unity app can decode there), and that results handed back through a
// ConcurrentQueue, drained on the main thread, are complete. That is the
// pattern documented at the top of csharp/UniNet/Session.cs. Run:
//
//     dotnet run --project tests/interop/InteropCs -- <realm> [seconds]
//
// The native library must be findable: put libuninet_c.so next to the binary or
// set LD_LIBRARY_PATH.
using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text.Json;
using System.Threading;
using UniNet;

static class Program
{
    // The one payload all three languages must produce identically. Written as
    // literal JSON rather than serialized from an object so that C#'s
    // serializer conventions cannot quietly change it.
    static string Payload(string lang) =>
        "{\"from\":\"" + lang + "\",\"text\":\"Röntgen 20°C\",\"exact\":0.5," +
        "\"inexact\":3.25,\"count\":42,\"neg\":-7,\"flag\":true,\"nothing\":null," +
        "\"pts\":[1.5,2.5,3.5],\"nested\":{\"a\":1,\"b\":[true,false]}}";

    // The discovery headers each language advertises; see interop_cpp.cpp.
    static Dictionary<string, string> HeadersFor(string lang)
    {
        string pid = new string('0', 30) + (lang == "cpp" ? "c1" : lang == "python" ? "b2" : "c3");
        return new Dictionary<string, string>
        {
            ["tn.proto"] = "1.1",
            ["tn.kind"] = lang,
            ["tn.pid"] = pid,
            ["tn.name"] = lang + " Röntgen",
            ["tn.caps"] = "presence,align," + lang,
        };
    }

    static string CheckHeaders(Peer peer, string lang)
    {
        foreach (var kv in HeadersFor(lang))
        {
            if (!peer.HasHeader(kv.Key)) return $"header '{kv.Key}' missing";
            if (peer.Header(kv.Key) != kv.Value)
                return $"header '{kv.Key}': got '{peer.Header(kv.Key)}', expected '{kv.Value}'";
        }
        return "";
    }

    // Compare every field except "from", which necessarily differs per sender.
    static string Check(string json)
    {
        using var got = JsonDocument.Parse(json);
        using var want = JsonDocument.Parse(Payload("x"));

        foreach (var expected in want.RootElement.EnumerateObject())
        {
            if (expected.Name == "from") continue;
            if (!got.RootElement.TryGetProperty(expected.Name, out var actual))
                return $"field '{expected.Name}' missing";

            // Compare the canonical text of each value. Numbers are the risk:
            // 0.5 must not arrive as 0.50000000000000001, and 42 must not
            // arrive as 42.0.
            string a = actual.GetRawText();
            string e = expected.Value.GetRawText();
            if (a != e)
            {
                // Tolerate an equal number written differently (e.g. 3.25 vs 3.2500).
                if (actual.ValueKind == JsonValueKind.Number &&
                    expected.Value.ValueKind == JsonValueKind.Number &&
                    double.TryParse(a, NumberStyles.Float, CultureInfo.InvariantCulture, out var da) &&
                    double.TryParse(e, NumberStyles.Float, CultureInfo.InvariantCulture, out var de) &&
                    Math.Abs(da - de) < 1e-12)
                {
                    // An integer must still be an integer, not a float.
                    bool wantInt = !e.Contains('.') && !e.Contains('e') && !e.Contains('E');
                    bool gotInt = !a.Contains('.') && !a.Contains('e') && !a.Contains('E');
                    if (wantInt && !gotInt)
                        return $"field '{expected.Name}': integer arrived as {a}";
                    continue;
                }
                return $"field '{expected.Name}': got {a}, expected {e}";
            }
        }
        return "";
    }

    static int Main(string[] args)
    {
        if (args.Length < 1)
        {
            Console.Error.WriteLine("usage: InteropCs <realm> [seconds] [expected-peers-csv]");
            return 2;
        }
        string realm = args[0];
        int seconds = args.Length > 1 ? int.Parse(args[1]) : 20;
        // A language that is not installed is passed out of this list rather
        // than counted as a failure: a skipped participant is a coverage gap.
        string[] expected = (args.Length > 2 ? args[2] : "cpp,python")
            .Split(',', StringSplitOptions.RemoveEmptyEntries);

        // marshalToCaller: false. This is a console app with no main-thread
        // requirement, so events are taken directly on the delivery thread.
        int mainThread = Thread.CurrentThread.ManagedThreadId;
        var headerResults = new Dictionary<string, string>();      // from Peers()
        var foundResults = new Dictionary<string, string>();       // from PeerFound
        var gate = new object();

        using var net = Session.Join("csharp", role: "interop", app: "csharp",
                                     realm: realm, marshalToCaller: false,
                                     headers: HeadersFor("csharp"));
        if (!net.Connected)
        {
            Console.Error.WriteLine("csharp: could not join the network");
            return 1;
        }

        var results = new Dictionary<string, string>();

        // Let the others arrive and start sending before anything is
        // registered, as a Unity scene that subscribes in Start() would. Their
        // hellos are then held natively for the first subscription, and their
        // PeerFound events have already happened: the late-registration paths
        // below are the ones exercised.
        var settleIn = DateTime.UtcNow.AddSeconds(Math.Min(10, seconds / 2));
        while (DateTime.UtcNow < settleIn && net.Peers().Count < expected.Length)
            Thread.Sleep(100);
        Thread.Sleep(1000);

        // PeerFound carries the headers too, not only Peers(), and a handler
        // added after the peers arrived still hears about them (the replay).
        net.PeerFound += peer =>
        {
            if (Array.IndexOf(expected, peer.Name) < 0) return;
            lock (gate)
            {
                if (!foundResults.ContainsKey(peer.Name))
                    foundResults[peer.Name] = CheckHeaders(peer, peer.Name);
            }
        };

        // Decoding off the main thread: the handler decodes (CBOR -> JSON ->
        // check) on a UniNet thread and only hands the verdict over. The hellos
        // held since the join are delivered during this very call; they too
        // must run off the main thread.
        var decoded = new ConcurrentQueue<(string Sender, string Verdict)>();
        int onMainThread = 0, offMainThread = 0, duringSubscribe = 0;
        bool subscribing = true;
        net.SubscribeCbor("interop.hello", (subject, src, cbor) =>
        {
            if (Volatile.Read(ref subscribing)) Interlocked.Increment(ref duringSubscribe);
            if (Thread.CurrentThread.ManagedThreadId == mainThread)
                Interlocked.Increment(ref onMainThread);
            else
                Interlocked.Increment(ref offMainThread);
            string json = Session.CborToJson(cbor);
            using var doc = JsonDocument.Parse(json);
            if (!doc.RootElement.TryGetProperty("from", out var from)) return;
            decoded.Enqueue((from.GetString() ?? "", Check(json)));
        });
        Volatile.Write(ref subscribing, false);
        var cborResults = new Dictionary<string, string>();   // main thread only

        net.Subscribe("interop.hello", msg =>
        {
            using var doc = JsonDocument.Parse(msg.Json);
            if (!doc.RootElement.TryGetProperty("from", out var from)) return;
            string sender = from.GetString() ?? "";
            if (sender.Length == 0) return;
            lock (gate)
            {
                if (!results.ContainsKey(sender)) results[sender] = Check(msg.Json);
            }
        });

        // Republish while waiting: the others start at different moments.
        // Once satisfied, keep publishing through a short settle period rather
        // than exiting immediately: leaving the moment WE have heard everyone
        // tears down the session while the others may still be waiting on OUR
        // payload.
        var deadline = DateTime.UtcNow.AddSeconds(seconds);
        DateTime? settleUntil = null;
        while (DateTime.UtcNow < deadline)
        {
            net.Publish("interop.hello", Payload("csharp"));
            // The main-thread half of the handoff: what Update() does in Unity.
            while (decoded.TryDequeue(out var d))
                if (d.Sender.Length > 0 && !cborResults.ContainsKey(d.Sender))
                    cborResults[d.Sender] = d.Verdict;
            foreach (var peer in net.Peers())
                if (Array.IndexOf(expected, peer.Name) >= 0 && !headerResults.ContainsKey(peer.Name))
                    headerResults[peer.Name] = CheckHeaders(peer, peer.Name);
            lock (gate)
            {
                if (results.Count >= expected.Length && headerResults.Count >= expected.Length &&
                    foundResults.Count >= expected.Length &&
                    expected.All(cborResults.ContainsKey) && settleUntil is null)
                    settleUntil = DateTime.UtcNow.AddSeconds(3);
            }
            if (settleUntil is not null && DateTime.UtcNow >= settleUntil) break;
            Thread.Sleep(300);
        }

        int failures = 0;
        lock (gate)
        {
            foreach (var kv in results.OrderBy(k => k.Key))
            {
                if (kv.Value.Length == 0)
                    Console.WriteLine($"csharp: PASS payload from {kv.Key} matched");
                else
                {
                    Console.WriteLine($"csharp: FAIL payload from {kv.Key}: {kv.Value}");
                    failures++;
                }
            }
            foreach (var lang in expected)
                if (!results.ContainsKey(lang))
                {
                    Console.WriteLine($"csharp: MISSING never heard from {lang}");
                    failures++;
                }
            failures += Report("tn.* headers in Peers()", headerResults, expected);
            failures += Report("tn.* headers on PeerFound", foundResults, expected);
        }
        failures += Report("CBOR decoded off the main thread", cborResults, expected);
        if (onMainThread != 0 || offMainThread == 0)
        {
            Console.WriteLine($"csharp: FAIL SubscribeCbor ran {onMainThread} handler(s) on the " +
                              $"main thread and {offMainThread} off it");
            failures++;
        }
        else
        {
            Console.WriteLine($"csharp: PASS all {offMainThread} SubscribeCbor handlers ran off " +
                              $"the main thread (marshalToCaller: false), {duringSubscribe} of " +
                              "them messages held from before the subscription");
        }
        if (duringSubscribe == 0)
            Console.WriteLine("csharp: NOTE no message was held before SubscribeCbor, so the " +
                              "buffered path was not exercised this run");

        Console.WriteLine($"csharp: {(failures == 0 ? "ALL OK" : "FAILED")}");
        return failures == 0 ? 0 : 1;
    }

    // One PASS/FAIL/MISSING line per expected peer; returns the failures.
    static int Report(string what, Dictionary<string, string> got, string[] expected)
    {
        int failures = 0;
        foreach (var lang in expected)
        {
            if (!got.TryGetValue(lang, out var why))
            {
                Console.WriteLine($"csharp: MISSING {what} from {lang}");
                failures++;
            }
            else if (why.Length == 0)
                Console.WriteLine($"csharp: PASS {what} from {lang} matched");
            else
            {
                Console.WriteLine($"csharp: FAIL {what} from {lang}: {why}");
                failures++;
            }
        }
        return failures;
    }
}
