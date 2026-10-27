/* (c) Teeworlds Archive Project Contributors.                                               */
/* (c) Teeworlds LastDay - Bamcane.                                                          */
/* This is a modified version of Teeworlds - see license.txt for details.                    */
#ifndef BASE_TL_STREAM_H
#define BASE_TL_STREAM_H

#include <base/system/fs.h>
#include <base/system/io.h>
#include <base/system/mem.h>

#include "array.h"

// A stream is either writable, readable or both. The writing half is pure
// virtual because every existing stream writes; the reading half defaults to
// "this stream cannot be read", so a write only stream (memory_stream,
// inplace_memory_stream) keeps working unchanged while a reader can accept any
// stream and only has to check can_read().
//
// Reading is positional: read() advances the position, seek()/tell()/size()
// make random access possible, which is what a datafile needs. A reader must
// treat a short read (fewer bytes than asked for) as the end of the stream.
enum EStreamSeek
{
	STREAM_SEEK_START = 0,
	STREAM_SEEK_CUR = 1,
	STREAM_SEEK_END = 2,
};

class stream
{
public:
	stream() {};
	virtual ~stream() {};

	virtual unsigned write(const unsigned char *buffer, unsigned size) = 0;
	virtual unsigned write_newline() = 0;

	virtual bool can_read() const { return false; }
	virtual unsigned read(unsigned char *pBuffer, unsigned Size) { return 0; }
	virtual bool seek(int Offset, EStreamSeek Origin = STREAM_SEEK_START) { return false; }
	virtual int tell() const { return -1; }
	// total size in bytes, or -1 when the stream has no known size
	virtual int size() const { return -1; }
};

class file_stream : public stream
{
	IOHANDLE file;
	bool auto_io_close;

public:
	file_stream(IOHANDLE handle, bool auto_close = true)
	{
		file = handle;
		auto_io_close = auto_close;
	}

	virtual ~file_stream()
	{
		if(auto_io_close && file)
			io_close(file);
	}

	virtual unsigned write(const unsigned char *buffer, unsigned size) override
	{
		if(!file)
			return 0;
		return io_write(file, buffer, size);
	}

	virtual unsigned write_newline() override
	{
		if(!file)
			return 0;
		return io_write_newline(file);
	}

	virtual bool can_read() const override { return file != 0; }

	virtual unsigned read(unsigned char *pBuffer, unsigned Size) override
	{
		if(!file)
			return 0;
		return io_read(file, pBuffer, Size);
	}

	virtual bool seek(int Offset, EStreamSeek Origin = STREAM_SEEK_START) override
	{
		if(!file)
			return false;
		const int OriginInt = Origin == STREAM_SEEK_END ? IOSEEK_END : (Origin == STREAM_SEEK_CUR ? IOSEEK_CUR : IOSEEK_START);
		return io_seek(file, Offset, OriginInt) == 0;
	}

	virtual int tell() const override
	{
		if(!file)
			return -1;
		return (int) io_tell(file);
	}

	virtual int size() const override
	{
		if(!file)
			return -1;
		return (int) io_length(file);
	}
};

// A read only window over memory that already exists (a decompressed pack
// blob, a map loaded for the server). The buffer is borrowed: the caller keeps
// it alive for the lifetime of the stream.
class memory_read_stream : public stream
{
	const unsigned char *m_pData;
	unsigned m_Size;
	unsigned m_Pos;
	bool m_Owns;

public:
	// A read only window over memory that already exists (a decompressed pack
	// blob, a buffer the caller keeps alive). The buffer is borrowed.
	memory_read_stream(const void *pData, unsigned Size)
	{
		m_pData = (const unsigned char *) pData;
		m_Size = Size;
		m_Pos = 0;
		m_Owns = false;
	}

	// A read only stream over a buffer that was allocated with mem_alloc and
	// belongs to the stream from now on. The caller may free its own copy.
	memory_read_stream(const void *pData, unsigned Size, bool Owns)
	{
		m_pData = (const unsigned char *) pData;
		m_Size = Size;
		m_Pos = 0;
		m_Owns = Owns;
	}

	virtual ~memory_read_stream()
	{
		if(m_Owns && m_pData)
			mem_free((void *) m_pData);
	}

	virtual unsigned write(const unsigned char *buffer, unsigned size) override { return 0; }
	virtual unsigned write_newline() override { return 0; }

	virtual bool can_read() const override { return m_pData != 0; }

	virtual unsigned read(unsigned char *pBuffer, unsigned Size) override
	{
		if(!m_pData || m_Pos >= m_Size)
			return 0;
		unsigned Count = Size;
		if(Count > m_Size - m_Pos)
			Count = m_Size - m_Pos;
		mem_copy(pBuffer, m_pData + m_Pos, Count);
		m_Pos += Count;
		return Count;
	}

	virtual bool seek(int Offset, EStreamSeek Origin = STREAM_SEEK_START) override
	{
		long long Target = Offset;
		if(Origin == STREAM_SEEK_CUR)
			Target += (long long) m_Pos;
		else if(Origin == STREAM_SEEK_END)
			Target += (long long) m_Size;
		if(Target < 0 || Target > (long long) m_Size)
			return false;
		m_Pos = (unsigned) Target;
		return true;
	}

	virtual int tell() const override { return (int) m_Pos; }
	virtual int size() const override { return (int) m_Size; }
};

class inplace_memory_stream : public stream
{
	unsigned char *start;
	unsigned char *end;

public:
	inplace_memory_stream(unsigned char *memory, unsigned size)
	{
		start = memory;
		end = start + size;
	}

	virtual unsigned write(const unsigned char *buffer, unsigned size) override
	{
		if(start + size > end)
			return 0;
		mem_copy(start, buffer, size);
		return size;
	}

	virtual unsigned write_newline() override
	{
		if(start >= end)
			return 0;
		*start = '\n';
		start++;
		return 1;
	}
};

template<class T>
class memory_stream : public stream
{
	static_assert(sizeof(T) == 1, "the size of buffer element should be 1 byte");
	array<T> *mem_buffer;

public:
	memory_stream(array<T> *buffer)
	{
		mem_buffer = buffer;
	}

	virtual unsigned write(const unsigned char *buffer, unsigned size) override
	{
		const int start = mem_buffer->size();
		// append() grows geometrically; set_size() reallocates and copies the
		// whole buffer on every write, making chunked writes O(n^2)
		mem_buffer->append((int) size);
		inplace_memory_stream stream((unsigned char *) mem_buffer->base_ptr() + start, size);
		return stream.write(buffer, size);
	}

	virtual unsigned write_newline() override
	{
		return write((const unsigned char *) ("\n"), 1);
	}
};

#endif // BASE_TL_STREAM_H
