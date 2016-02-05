#include "PipeFileTar.h"
#include <algorithm>
#include "../common/data.h"
#include "PipeSessions.h"
#include "../common/adler32.h"
#include "FileServ.h"
#include "../urbackupcommon/os_functions.h"
#include "FileMetadataPipe.h"
#include "../stringtools.h"

#ifndef _S_IFIFO
#define	_S_IFIFO 0x1000
#endif
#ifndef _S_IFCHR
#define	_S_IFCHR 0x2000
#endif
#ifndef _S_IFBLK
#define	_S_IFBLK 0x3000
#endif

namespace
{
	bool is_zeroes(const std::string& str)
	{
		for (size_t i = 0; i < str.size(); ++i)
		{
			if (str[i] != 0)
			{
				return false;
			}
		}
		return true;
	}

	int64 roundUp(int64 numToRound, int64 multiple)
	{
		return ((numToRound + multiple - 1) / multiple) * multiple;
	}

	std::string extract_string(const std::string& header, size_t off, size_t size)
	{
		for (size_t i = off; i < off+size; ++i)
		{
			if (header[i] == 0)
			{
				return header.substr(off, i - off);
			}
		}
		return header.substr(off, size);
	}

	std::string extract_digits(const std::string& val)
	{
		std::string ret;
		for (size_t i = 0; i < val.size(); ++i)
		{
			if ((val[i] >= '0' && val[i] <= '8') || val[i] == '-')
			{
				ret += val[i];
			}
		}
		return ret;
	}

	int64 extract_number(const std::string& header, size_t off, size_t size)
	{
		std::string val = extract_string(header, off, size);

		if (!val.empty() && val[0] < 0)
		{
			unsigned char f = val[0];
			bool neg = (f & 0x40)>0;
			int64 res = f & 0x3F;

			for (size_t i = 1; i < val.size(); ++i)
			{
				f = val[i];
				if (f == 0)
				{
					break;
				}
				res = (res << 8) | f;
			}

			if (neg)
			{
				res *= -1;
			}

			return res;
		}

		return strtoll(extract_digits(val).c_str(), NULL, 8);
	}

	bool check_header_checksum(const std::string& header)
	{
		int64 checksum = extract_number(header, 148, 8);

		int64 checksum1 = 0;
		int64 checksum2 = 0;
		for (size_t i = 0; i < header.size(); ++i)
		{
			if (i >= 148 && i < 148 + 8)
			{
				checksum1 += 32;
				checksum2 += 32;
			}
			else
			{
				checksum1 += header[i];
				checksum2 += static_cast<unsigned char>(header[i]);
			}
		}

		if (checksum1 != checksum && checksum2 != checksum)
		{
			return false;
		}
		else
		{
			return true;
		}
	}
}

PipeFileTar::PipeFileTar(const std::string & pCmd, int backupnum, int64 fn_random, std::string output_fn)
	: pipe_file(new PipeFileStore(new PipeFile(pCmd))), file_offset(0), mutex(Server->createMutex()), backupnum(backupnum), has_next(false), output_fn(output_fn), fn_random(fn_random)
{
	sha_def_init(&sha_ctx);
}

PipeFileTar::PipeFileTar(PipeFileStore* pipe_file, const STarFile tar_file, int64 file_offset, int backupnum, int64 fn_random, std::string output_fn)
	: pipe_file(pipe_file), tar_file(tar_file), file_offset(file_offset), mutex(Server->createMutex()), backupnum(backupnum), has_next(false), output_fn(output_fn), fn_random(fn_random)
{
	sha_def_init(&sha_ctx);
}

PipeFileTar::~PipeFileTar()
{
	pipe_file->decr();
	if (pipe_file->refcount == 0)
	{
		delete pipe_file->pipe_file;
		delete pipe_file;
	}
}

std::string PipeFileTar::Read(_u32 tr, bool * has_error)
{
	IScopedLock lock(mutex.get());

	int64 pos = tar_file.pos;

	lock.relock(NULL);

	std::string ret = Read(pos, tr, has_error);

	lock.relock(mutex.get());

	sha_def_update(&sha_ctx, reinterpret_cast<const unsigned char*>(ret.data()), ret.size());

	tar_file.pos += ret.size();
	return ret;
}

std::string PipeFileTar::Read(int64 spos, _u32 tr, bool * has_error)
{
	IScopedLock lock(mutex.get());

	if (!tar_file.available)
	{
		if (has_error) *has_error = true;
		return std::string();
	}

	_u32 max_read = static_cast<_u32>((std::max)(tar_file.size - spos, static_cast<int64>(tr)));
	int64 pf_offset = file_offset + spos;

	lock.relock(NULL);

	std::string ret = pipe_file->pipe_file->Read(pf_offset, max_read, has_error);

	lock.relock(mutex.get());
	sha_def_update(&sha_ctx, reinterpret_cast<const unsigned char*>(ret.data()), ret.size());

	return ret;
}

_u32 PipeFileTar::Read(char * buffer, _u32 bsize, bool * has_error)
{
	IScopedLock lock(mutex.get());

	int64 pos = tar_file.pos;

	lock.relock(NULL);

	_u32 ret = Read(tar_file.pos, buffer, bsize, has_error);

	lock.relock(mutex.get());

	sha_def_update(&sha_ctx, reinterpret_cast<const unsigned char*>(buffer), ret);
	
	tar_file.pos += ret;
	return ret;
}

_u32 PipeFileTar::Read(int64 spos, char * buffer, _u32 bsize, bool * has_error)
{
	IScopedLock lock(mutex.get());

	if (!tar_file.available)
	{
		if (has_error) *has_error = true;
		return 0;
	}

	int64 pf_offset = file_offset + spos;

	lock.relock(NULL);

	_u32 ret = pipe_file->pipe_file->Read(pf_offset, buffer, bsize, has_error);

	lock.relock(mutex.get());
	sha_def_update(&sha_ctx, reinterpret_cast<const unsigned char*>(buffer), ret);

	return ret;
}

bool PipeFileTar::Seek(_i64 spos)
{
	IScopedLock lock(mutex.get());
	if (spos > tar_file.size || spos<0)
	{
		return false;
	}

	tar_file.pos = spos;

	int64 pf_offset = file_offset + spos;

	lock.relock(NULL);

	return pipe_file->pipe_file->Seek(pf_offset);
}

bool PipeFileTar::switchNext(std::string& fn, bool& is_dir, bool& is_symlink, bool& is_special, std::string& symlink_target, int64& size, bool *has_error)
{
	IScopedLock lock(mutex.get());

	bool b= readHeader(has_error);
	if (b)
	{
		fn = tar_file.fn;
		is_dir = tar_file.is_dir;
		is_symlink = tar_file.is_symlink;
		is_special = tar_file.is_special;
		symlink_target = tar_file.symlink_target;
		size = tar_file.size;
	}
	return b;
}

_i64 PipeFileTar::Size()
{
	IScopedLock lock(mutex.get());

	return tar_file.size;
}

std::string PipeFileTar::buildCurrMetadata()
{
	CWData data;
	data.addChar(ID_METADATA_V1);
	_u32 fn_start= data.getDataSize();

	std::string type;
	if (tar_file.is_dir && tar_file.is_symlink)
	{
		type = "l";
	}
	else if (tar_file.is_dir)
	{
		type = "d";
	}
	else
	{
		type = "f";
	}

	data.addString(type + tar_file.fn);
	data.addUInt(urb_adler32(urb_adler32(0, NULL, 0), data.getDataPtr()+ fn_start, static_cast<_u32>(data.getDataSize())- fn_start));
	_u32 common_start = data.getDataSize();
	data.addChar(1);
	data.addVarInt(0);
	data.addVarInt(tar_file.buf.st_mtime);
	data.addVarInt(0);
	data.addVarInt(0);
	data.addVarInt(0);
	std::auto_ptr<IFileServ::ITokenCallback> token_callback(FileServ::newTokenCallback());
	if (token_callback.get() != NULL)
	{
		data.addString(token_callback->translateTokens(tar_file.buf.st_uid, tar_file.buf.st_gid, tar_file.buf.st_mode));
	}
	else
	{
		data.addString(std::string());
	}
	data.addUInt(urb_adler32(urb_adler32(0, NULL, 0), data.getDataPtr()+ common_start, static_cast<_u32>(data.getDataSize())- common_start));
	_u32 os_start = data.getDataSize();

#ifdef _WIN32
	data.addChar(1);
	data.addUInt(0); //atributes
	data.addVarInt(0); //creation time
	data.addVarInt(0); //last access time
	data.addVarInt(os_to_windows_filetime(tar_file.buf.st_mtime)); //modify time
	data.addVarInt(os_to_windows_filetime(tar_file.buf.st_mtime)); //ctime
	data.addChar(0);
	//TODO: Symlink etc.
#else
	serialize_stat_buf(tar_file.buf, tar_file.symlink_target, data);
	data.addInt64(0);
#endif
	data.addUInt(urb_adler32(urb_adler32(0, NULL, 0), data.getDataPtr() + os_start, static_cast<_u32>(data.getDataSize())- os_start));

	return std::string(data.getDataPtr(), data.getDataSize());
}

bool PipeFileTar::readHeader(bool* has_error)
{
	std::string header = pipe_file->pipe_file->Read(file_offset+ roundUp(tar_file.size, 512), 512);

	if (header.size() != 512)
	{
		if(has_error!=NULL) *has_error = true;
		return false;
	}

	if (is_zeroes(header))
	{
		header = pipe_file->pipe_file->Read(512);

		if (header.size() != 512)
		{
			if (has_error != NULL) *has_error = true;
			return false;
		}

		if (is_zeroes(header))
		{
			return false;
		}
	}

	if (!check_header_checksum(header))
	{
		if (has_error != NULL) *has_error = true;
		return false;
	}

	file_offset += roundUp(tar_file.size, 512);
	file_offset += 512;

	tar_file.available = true;
	tar_file.fn = extract_string(header, 0, 100);
	tar_file.size = extract_number(header, 124, 12);
	tar_file.buf.st_mode = extract_number(header, 100, 8);
	tar_file.buf.st_uid = extract_number(header, 108, 8);
	tar_file.buf.st_gid = extract_number(header, 116, 8);
	tar_file.buf.st_mtime = extract_number(header, 136, 8);
	tar_file.buf.st_atime = 0;
	tar_file.pos = 0;

	char type = header[156];

	tar_file.is_dir = (!tar_file.fn.empty() && tar_file.fn[tar_file.fn.size() - 1] == '/');

	if (!tar_file.is_dir)
	{
		int abc = 5;
	}

	tar_file.is_special = !tar_file.is_dir && (type != '0' && type!=0);
	tar_file.is_symlink = false;

	if (type == '2')
	{
		tar_file.symlink_target = extract_string(header, 157, 100);
		tar_file.is_symlink = true;
		tar_file.is_special = true;
	}

	if (extract_string(header, 257, 5) == "ustar")
	{
		tar_file.buf.st_dev = extract_number(header, 329, 8) << 8 | extract_number(header, 337, 8);
		std::string prefix = extract_string(header, 345, 155);
		if (!prefix.empty())
		{
			tar_file.fn = prefix+"/"+tar_file.fn;
		}

		tar_file.is_dir = false;

		if (type == '3')
		{
			tar_file.buf.st_mode |= _S_IFCHR;
		}
		else if (type == '4')
		{
			tar_file.buf.st_mode |= _S_IFBLK;
		}
		else if (type == '5')
		{
			tar_file.is_dir = true;
			tar_file.is_special = false;
		}
		else if (type == '6')
		{
			tar_file.buf.st_mode |= _S_IFIFO;
		}
	}

	return true;
}

_u32 PipeFileTar::Write(const std::string & tw, bool * has_error)
{
	return 0;
}

_u32 PipeFileTar::Write(int64 spos, const std::string & tw, bool * has_error)
{
	return 0;
}

_u32 PipeFileTar::Write(const char * buffer, _u32 bsiz, bool * has_error)
{
	return 0;
}

_u32 PipeFileTar::Write(int64 spos, const char * buffer, _u32 bsiz, bool * has_error)
{
	return 0;
}

_i64 PipeFileTar::RealSize()
{
	return Size();
}

bool PipeFileTar::PunchHole(_i64 spos, _i64 size)
{
	return false;
}

bool PipeFileTar::Sync()
{
	return false;
}

std::string PipeFileTar::getFilename(void)
{
	IScopedLock lock(mutex.get());
	return tar_file.fn;
}

int64 PipeFileTar::getLastRead()
{
	return pipe_file->pipe_file->getLastRead();
}

bool PipeFileTar::getHasError()
{
	return pipe_file->pipe_file->getHasError();
}

std::string PipeFileTar::getStdErr()
{
	std::string stderr_ret;

	unsigned char dig[SHA_DEF_DIGEST_SIZE];
	sha_def_final(&sha_ctx, dig);

	stderr_ret.append(1, 1);

	stderr_ret.append(reinterpret_cast<const char*>(dig),
		reinterpret_cast<const char*>(dig) + SHA_DEF_DIGEST_SIZE);

	std::string fn;
	bool is_dir;
	bool is_symlink;
	bool is_special;
	std::string symlink_target;
	int64 size;


	while (true)
	{
		if(switchNext(fn, is_dir, is_symlink, is_special, symlink_target, size))
		{
			if (!fn.empty() && fn[fn.size() - 1] == '/')
			{
				fn.resize(fn.size() - 1);
			}
			
			if (fn == ".")
			{
				fn.clear();
			}

			if (next(fn, 0, "./"))
			{
				fn = fn.substr(2);
			}

			CWData data;
			data.addString(output_fn + (fn.empty() ? "" : ("/" + fn)));
			data.addChar(is_dir ? 1 : 0);
			data.addChar(is_symlink ? 1 : 0);
			data.addChar(is_special ? 1 : 0);
			data.addString(symlink_target);
			data.addVarInt(size);
			data.addUInt(static_cast<unsigned int>(fn_random));

			CWData header;
			header.addChar(2);
			header.addUInt(data.getDataSize());

			stderr_ret.append(header.getDataPtr(), header.getDataSize());
			stderr_ret.append(data.getDataPtr(), data.getDataSize());

			std::string remote_fn = output_fn + (fn.empty() ? "" : ("/" + fn)) + "|" + convert(backupnum) + "|" + convert(fn_random);

			if (!is_dir && !is_symlink && !is_special)
			{
				pipe_file->inc();
				has_next = true;
				PipeSessions::injectPipeSession(remote_fn,
					backupnum, new PipeFileTar(pipe_file, tar_file, file_offset, backupnum, fn_random, output_fn), buildCurrMetadata());
				return stderr_ret;
			}
			else
			{
				PipeSessions::injectPipeSession(remote_fn,
					backupnum, NULL, buildCurrMetadata());
			}
		}
		else
		{
			break;
		}
	}

	stderr_ret += pipe_file->pipe_file->getStdErr();

	return stderr_ret;
}

bool PipeFileTar::getExitCode(int & exit_code)
{
	if (has_next)
	{
		exit_code = 0;
		return true;
	}
	else
	{
		return pipe_file->pipe_file->getExitCode(exit_code);
	}
}

void PipeFileTar::forceExitWait()
{
	if (!has_next)
	{
		pipe_file->pipe_file->forceExitWait();
	}
}

void PipeFileTar::addUser()
{
	pipe_file->pipe_file->addUser();
}

void PipeFileTar::removeUser()
{
	pipe_file->pipe_file->removeUser();
}

bool PipeFileTar::hasUser()
{
	return pipe_file->pipe_file->hasUser();
}