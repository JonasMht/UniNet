// UniNet cross-language interop: the C# participant.
//
// See interop_cpp.cpp for what this proves. Run:
//
//     dotnet run --project tests/interop/InteropCs -- <realm> [seconds]
//
// The native library must be findable: put libuninet_c.so next to the binary or
// set LD_LIBRARY_PATH.
using System;
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
        // requirement, so events are taken directly on the network thread.
        using var net = Session.Join("csharp", role: "interop", app: "csharp",
                                     realm: realm, marshalToCaller: false);
        if (!net.Connected)
        {
            Console.Error.WriteLine("csharp: could not join the network");
            return 1;
        }

        var results = new Dictionary<string, string>();
        var gate = new object();

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
            lock (gate)
            {
                if (results.Count >= expected.Length && settleUntil is null)
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
        }

        Console.WriteLine($"csharp: {(failures == 0 ? "ALL OK" : "FAILED")}");
        return failures == 0 ? 0 : 1;
    }
}
