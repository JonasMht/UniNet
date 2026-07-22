// UniNet C# demo: two peers on one loopback bus exchange messages. Run after
// building the native lib and placing libuninet_c.so next to the demo binary
// (or LD_LIBRARY_PATH it). Demonstrates P/Invoke end-to-end pub/sub.
using UniNet;

string lib = (Environment.GetEnvironmentVariable("UNINET_LIB") ?? "libuninet_c.so");
Console.WriteLine($"UniNet: protocol {Build.ProtocolVersion}, lz4={Build.HasLz4}, nats={Build.HasNats}");

using var bus = new LoopbackTransport();
using var alice = new Node("alice", bus);
using var bob = new Node("bob", bus);

bob.Subscribe("domain.D1", (subj, text) => Console.WriteLine($"bob heard on {subj}: {text}"));

alice.Publish("domain.D1", "hello from C# over UniNet");
alice.Publish("domain.D1", "and a second frame");
Console.WriteLine($"alice uuid: {alice.Uuid}");
Console.WriteLine($"bob   uuid: {bob.Uuid}");
