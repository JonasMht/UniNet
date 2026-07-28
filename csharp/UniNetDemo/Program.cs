// UniNet C# demo: two devices finding each other and talking, with nothing
// configured by anyone.
//
//   dotnet run --project csharp/UniNetDemo
//
// Place libuninet_c.so / uninet_c.dll next to the executable first, or point
// LD_LIBRARY_PATH (Linux) / PATH (Windows) at the directory holding it.
using System;
using System.Threading;
using UniNet;

class Program
{
    static int Main()
    {
        Console.WriteLine($"UniNet {Session.Version}");
        Console.WriteLine($"protocol v{Session.ProtocolVersion}, LZ4: {Session.HasLz4}\n");

        // A realm unique to this run, so the demo cannot collide with a real
        // session (or with a second copy of itself) on the same network.
        // Environment.ProcessId is .NET 5+; this project targets netstandard2.1 so that
        // Unity can consume it, where only the older API exists.
        int pid = System.Diagnostics.Process.GetCurrentProcess().Id;
        string realm = $"uninet-csharp-demo-{pid}";

        // ── the entire setup: a name, and nothing else ──
        using var server = Session.Join("Recorder", role: "server",
                                        app: "UniNetDemo", realm: realm,
                                        marshalToCaller: false);
        using var viewer = Session.Join("Laptop", role: "viewer",
                                        app: "UniNetDemo", realm: realm,
                                        marshalToCaller: false);

        Console.WriteLine(server.Describe());
        if (!server.Connected)
        {
            Console.Error.WriteLine("Could not get onto the network.");
            return 1;
        }

        server.PeerFound += p => Console.WriteLine($"[server] found  {p}");
        server.PeerLost  += p => Console.WriteLine($"[server] lost   {p}");

        viewer.Subscribe("demo.>", msg =>
            Console.WriteLine($"[viewer] {msg.Subject}: {msg.Json}"));

        // Wait for discovery, a network event, not a function call.
        for (int i = 0; i < 100 && server.Peers().Count == 0; ++i) Thread.Sleep(100);

        Console.WriteLine($"\n{server.Describe()}");
        foreach (var p in server.Peers())
            Console.WriteLine($"  · {p.Name}  {p.Endpoint}  {p.Role}");

        // Broadcast, then a private message to one device.
        Console.WriteLine();
        server.Publish("demo.hello", "{\"code\":\"update\",\"tick\":1}");
        Thread.Sleep(500);

        string target = viewer.Uuid;
        server.Publish("demo.private", "{\"only_for\":\"the laptop\"}", dst: target);
        Thread.Sleep(500);

        // JSON and CBOR are the same data, so a peer in any language sees this
        // identically.
        byte[] cbor = Session.JsonToCbor("{\"points\":[1.0,2.0,3.0]}");
        Console.WriteLine($"\n{cbor.Length} bytes of CBOR -> {Session.CborToJson(cbor)}");

        server.Publish("demo.mesh", Session.CborToJson(cbor));
        Thread.Sleep(500);

        Console.WriteLine("\nDone.");
        return 0;
    }
}
