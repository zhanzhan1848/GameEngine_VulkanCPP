using System;

namespace PrimalEditor_Avalonia.Native;

/// <summary>
/// MurmurHash3 x86_32（与引擎 PfxTypes.h::hash_name 同算法同种子），
/// 用于暴露参数名字段的 C#/C++ 对齐（PfxSetExposedParam）。
/// </summary>
public static class PfxHash
{
    private const uint Seed = 0x9E3779B9u;

    public static uint HashName(string name)
    {
        var data = System.Text.Encoding.UTF8.GetBytes(name);
        return MurmurHash3X8632(data, Seed);
    }

    public static uint MurmurHash3X8632(byte[] data, uint seed)
    {
        const uint c1 = 0xCC9E2D51u;
        const uint c2 = 0x1B873593u;
        uint h1 = seed;
        int length = data.Length;
        int nblocks = length / 4;

        static uint Rotl(uint x, int r) => (x << r) | (x >> (32 - r));

        for (int i = 0; i < nblocks; i++)
        {
            uint k1 = (uint)(data[i * 4] | (data[i * 4 + 1] << 8) | (data[i * 4 + 2] << 16) | (data[i * 4 + 3] << 24));
            k1 *= c1;
            k1 = Rotl(k1, 15);
            k1 *= c2;
            h1 ^= k1;
            h1 = Rotl(h1, 13);
            h1 = h1 * 5u + 0xE6546B64u;
        }

        uint tail = 0;
        int tailStart = nblocks * 4;
        switch (length & 3)
        {
            case 3:
                tail ^= (uint)(data[tailStart + 2] << 16);
                goto case 2;
            case 2:
                tail ^= (uint)(data[tailStart + 1] << 8);
                goto case 1;
            case 1:
                tail ^= data[tailStart];
                tail *= c1;
                tail = Rotl(tail, 15);
                tail *= c2;
                h1 ^= tail;
                break;
        }

        h1 ^= (uint)length;
        h1 ^= h1 >> 16;
        h1 *= 0x85EBCA6Bu;
        h1 ^= h1 >> 13;
        h1 *= 0xC2B2AE35u;
        h1 ^= h1 >> 16;
        return h1;
    }
}
