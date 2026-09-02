using System;
using System.Buffers;
using System.IO;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace Primal.CliFramework;

/// <summary>
/// Length-prefixed JSON frame codec over a Stream/Socket.
/// Frame = 4-byte little-endian payload length + UTF-8 JSON payload.
/// Guard: 16 MiB max frame — a desynced stream must fail fast, not OOM.
/// </summary>
public static class FrameIO
{
    public const int MaxFrameBytes = 16 * 1024 * 1024;

    public static async Task<byte[]> ReadFrameAsync(Stream stream, CancellationToken ct)
    {
        var header = await ReadExactAsync(stream, 4, ct).ConfigureAwait(false);
        uint length = (uint)(header[0] | (header[1] << 8) | (header[2] << 16) | (header[3] << 24));
        if (length > MaxFrameBytes)
            throw new IOException($"Frame too large ({length} bytes > {MaxFrameBytes})");
        if (length == 0)
            return Array.Empty<byte>();
        return await ReadExactAsync(stream, (int)length, ct).ConfigureAwait(false);
    }

    public static byte[] EncodeFrame(string json)
    {
        var payload = Encoding.UTF8.GetBytes(json);
        if ((uint)payload.Length > MaxFrameBytes)
            throw new IOException($"Frame too large ({payload.Length} bytes)");
        var frame = new byte[4 + payload.Length];
        frame[0] = (byte)payload.Length;
        frame[1] = (byte)(payload.Length >> 8);
        frame[2] = (byte)(payload.Length >> 16);
        frame[3] = (byte)(payload.Length >> 24);
        payload.CopyTo(frame, 4);
        return frame;
    }

    private static async Task<byte[]> ReadExactAsync(Stream stream, int count, CancellationToken ct)
    {
        var buffer = ArrayPool<byte>.Shared.Rent(count);
        try
        {
            int total = 0;
            while (total < count)
            {
                int read = await stream.ReadAsync(buffer.AsMemory(total, count - total), ct).ConfigureAwait(false);
                if (read == 0)
                    throw new EndOfStreamException($"Peer closed after {total}/{count} bytes");
                total += read;
            }
            var result = new byte[count];
            Buffer.BlockCopy(buffer, 0, result, 0, count);
            return result;
        }
        finally
        {
            ArrayPool<byte>.Shared.Return(buffer);
        }
    }
}
