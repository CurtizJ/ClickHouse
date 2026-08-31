#pragma once

#include <IO/ReadBuffer.h>
#include <IO/VarInt.h>

namespace DB
{

/// Reads a run of VarUInt values from a ReadBuffer.
/// Unlike a sequence of readVarUInt calls, it keeps the buffer position in a local
/// variable, so the compiler can hold it in a register across the values instead of
/// loading and storing the ReadBuffer members and checking the buffer bounds for
/// every value. The position is written back to the buffer in the destructor, so
/// the ReadBuffer must not be accessed directly while the reader is alive.
class VarUIntReader
{
public:
    explicit VarUIntReader(ReadBuffer & istr_) : istr(istr_)
    {
        pos = istr.position();
        end = istr.buffer().end();
    }

    ~VarUIntReader()
    {
        istr.position() = pos;
    }

    VarUIntReader(const VarUIntReader &) = delete;
    VarUIntReader & operator=(const VarUIntReader &) = delete;

    UInt64 ALWAYS_INLINE read()
    {
        if (static_cast<size_t>(end - pos) < max_varuint_size) [[unlikely]]
        {
            auto res = readNearBufferEnd(istr, pos);
            pos = res.pos;
            end = res.end;
            return res.value;
        }

        UInt64 x = 0;
        pos = decodeByte<0>(x, pos);
        return x;
    }

    void ALWAYS_INLINE ignore()
    {
        if (static_cast<size_t>(end - pos) < max_varuint_size) [[unlikely]]
        {
            auto res = readNearBufferEnd(istr, pos);
            pos = res.pos;
            end = res.end;
            return;
        }

        pos = skipByte<0>(pos);
    }

    /// Reads a VarUInt without any buffer bounds check. The caller must guarantee that
    /// the working buffer has enough bytes for the worst case (max_varuint_size per value),
    /// even against corrupted data, e.g. by checking available() against an upper bound
    /// of the total size of a batch of values before decoding it.
    UInt64 ALWAYS_INLINE readUnchecked()
    {
        UInt64 x = 0;
        pos = decodeByte<0>(x, pos);
        return x;
    }

    size_t ALWAYS_INLINE available() const { return static_cast<size_t>(end - pos); }

    /// Advances over num_bytes bytes, refilling the buffer as needed.
    void ALWAYS_INLINE skipBytes(size_t num_bytes)
    {
        if (num_bytes <= available()) [[likely]]
        {
            pos += num_bytes;
            return;
        }
        skipBytesSlow(num_bytes);
    }

    static constexpr size_t max_varuint_size = 10;

private:

    /// The decode loop is unrolled manually into a chain of loads with constant offsets
    /// and shifts by constant amounts: the loop form with an early exit is not unrolled
    /// by the optimizer and compiles to a longer dependency chain with variable shifts.
    template <size_t byte_idx>
    static ALWAYS_INLINE char * decodeByte(UInt64 & x, char * from)
    {
        UInt64 byte = static_cast<unsigned char>(from[byte_idx]);
        x |= (byte & 0x7F) << (7 * byte_idx);

        if constexpr (byte_idx + 1 == max_varuint_size)
        {
            return from + max_varuint_size;
        }
        else
        {
            if (!(byte & 0x80))
                return from + byte_idx + 1;
            return decodeByte<byte_idx + 1>(x, from);
        }
    }

    template <size_t byte_idx>
    static ALWAYS_INLINE char * skipByte(char * from)
    {
        if constexpr (byte_idx + 1 == max_varuint_size)
        {
            return from + max_varuint_size;
        }
        else
        {
            auto byte = static_cast<unsigned char>(from[byte_idx]);
            if (!(byte & 0x80))
                return from + byte_idx + 1;
            return skipByte<byte_idx + 1>(from);
        }
    }

    struct RefilledState
    {
        UInt64 value;
        char * pos;
        char * end;
    };

    /// The value may span the end of the working buffer, so read it through the checked
    /// ReadBuffer path, which refills the buffer as needed, and return the new bounds.
    /// Takes and returns all state by value: receiving `this` here would let the address
    /// of the reader escape and force `pos` into memory on the fast path as well.
    static RefilledState readNearBufferEnd(ReadBuffer & istr, char * current_pos)
    {
        istr.position() = current_pos;
        UInt64 x = 0;
        readVarUInt(x, istr);
        return {x, istr.position(), istr.buffer().end()};
    }

    void skipBytesSlow(size_t num_bytes)
    {
        istr.position() = pos;
        istr.ignore(num_bytes);
        pos = istr.position();
        end = istr.buffer().end();
    }

    ReadBuffer & istr;
    char * pos;
    char * end;
};

}
