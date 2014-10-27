// Copyright (c) 2013, Cloudera, inc.
// Confidential Cloudera Information: Covered by NDA.
// All rights reserved.
//
// Some portions copyright (C) 2008, Google, inc.
//
// Utilities for working with protobufs.
// Some of this code is cribbed from the protobuf source,
// but modified to work with kudu's 'faststring' instead of STL strings.

#include "kudu/util/pb_util.h"

#include <boost/foreach.hpp>
#include <glog/logging.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/io/zero_copy_stream.h>
#include <google/protobuf/message_lite.h>
#include <google/protobuf/message.h>
#include <string>
#include <tr1/memory>
#include <vector>

#include "kudu/gutil/bind.h"
#include "kudu/gutil/callback.h"
#include "kudu/gutil/strings/escaping.h"
#include "kudu/gutil/strings/fastmem.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/util/coding.h"
#include "kudu/util/coding-inl.h"
#include "kudu/util/crc.h"
#include "kudu/util/env.h"
#include "kudu/util/env_util.h"
#include "kudu/util/path_util.h"
#include "kudu/util/pb_util-internal.h"
#include "kudu/util/status.h"

using google::protobuf::FieldDescriptor;
using google::protobuf::Message;
using google::protobuf::MessageLite;
using google::protobuf::Reflection;
using kudu::crc::Crc;
using kudu::pb_util::internal::SequentialFileFileInputStream;
using kudu::pb_util::internal::WritableFileOutputStream;
using std::string;
using std::tr1::shared_ptr;
using std::vector;
using strings::Substitute;
using strings::Utf8SafeCEscape;

static const char* const kTmpTemplateSuffix = ".tmp.XXXXXX";

// Protobuf container constants.
static const int kPBContainerVersion = 1;
static const int kPBContainerMagicLen = 8;
static const int kPBContainerHeaderLen =
    // magic number + version
    kPBContainerMagicLen + sizeof(uint32_t);
static const int kPBContainerChecksumLen = sizeof(uint32_t);

namespace kudu {
namespace pb_util {

namespace {

// When serializing, we first compute the byte size, then serialize the message.
// If serialization produces a different number of bytes than expected, we
// call this function, which crashes.  The problem could be due to a bug in the
// protobuf implementation but is more likely caused by concurrent modification
// of the message.  This function attempts to distinguish between the two and
// provide a useful error message.
void ByteSizeConsistencyError(int byte_size_before_serialization,
                              int byte_size_after_serialization,
                              int bytes_produced_by_serialization) {
  CHECK_EQ(byte_size_before_serialization, byte_size_after_serialization)
      << "Protocol message was modified concurrently during serialization.";
  CHECK_EQ(bytes_produced_by_serialization, byte_size_before_serialization)
      << "Byte size calculation and serialization were inconsistent.  This "
         "may indicate a bug in protocol buffers or it may be caused by "
         "concurrent modification of the message.";
  LOG(FATAL) << "This shouldn't be called if all the sizes are equal.";
}

string InitializationErrorMessage(const char* action,
                                  const MessageLite& message) {
  // Note:  We want to avoid depending on strutil in the lite library, otherwise
  //   we'd use:
  //
  // return strings::Substitute(
  //   "Can't $0 message of type \"$1\" because it is missing required "
  //   "fields: $2",
  //   action, message.GetTypeName(),
  //   message.InitializationErrorString());

  string result;
  result += "Can't ";
  result += action;
  result += " message of type \"";
  result += message.GetTypeName();
  result += "\" because it is missing required fields: ";
  result += message.InitializationErrorString();
  return result;
}

} // anonymous namespace

bool AppendToString(const MessageLite &msg, faststring *output) {
  DCHECK(msg.IsInitialized()) << InitializationErrorMessage("serialize", msg);
  return AppendPartialToString(msg, output);
}

bool AppendPartialToString(const MessageLite &msg, faststring* output) {
  int old_size = output->size();
  int byte_size = msg.ByteSize();

  output->resize(old_size + byte_size);

  uint8* start = &((*output)[old_size]);
  uint8* end = msg.SerializeWithCachedSizesToArray(start);
  if (end - start != byte_size) {
    ByteSizeConsistencyError(byte_size, msg.ByteSize(), end - start);
  }
  return true;
}

bool SerializeToString(const MessageLite &msg, faststring *output) {
  output->clear();
  return AppendToString(msg, output);
}

bool ParseFromSequentialFile(MessageLite *msg, SequentialFile *rfile) {
  SequentialFileFileInputStream istream(rfile);
  return msg->ParseFromZeroCopyStream(&istream);
}

Status ParseFromArray(MessageLite* msg, const uint8_t* data, uint32_t length) {
  if (!msg->ParseFromArray(data, length)) {
    return Status::Corruption("Error parsing msg", InitializationErrorMessage("parse", *msg));
  }
  return Status::OK();
}

Status WritePBToPath(Env* env, const std::string& path,
                     const MessageLite& msg,
                     SyncMode sync) {
  const string tmp_template = path + kTmpTemplateSuffix;
  string tmp_path;

  gscoped_ptr<WritableFile> file;
  RETURN_NOT_OK(env->NewTempWritableFile(WritableFileOptions(), tmp_template, &tmp_path, &file));
  env_util::ScopedFileDeleter tmp_deleter(env, tmp_path);

  WritableFileOutputStream ostream(file.get());
  bool res = msg.SerializeToZeroCopyStream(&ostream);
  if (!res || !ostream.Flush()) {
    return Status::IOError("Unable to serialize PB to file");
  }

  if (sync == pb_util::SYNC) {
    RETURN_NOT_OK_PREPEND(file->Sync(), "Failed to Sync() " + tmp_path);
  }
  RETURN_NOT_OK_PREPEND(file->Close(), "Failed to Close() " + tmp_path);
  RETURN_NOT_OK_PREPEND(env->RenameFile(tmp_path, path), "Failed to rename tmp file to " + path);
  tmp_deleter.Cancel();
  if (sync == pb_util::SYNC) {
    RETURN_NOT_OK_PREPEND(env->SyncDir(DirName(path)), "Failed to SyncDir() parent of " + path);
  }
  return Status::OK();
}

Status ReadPBFromPath(Env* env, const std::string& path, MessageLite* msg) {
  shared_ptr<SequentialFile> rfile;
  RETURN_NOT_OK(env_util::OpenFileForSequential(env, path, &rfile));
  if (!ParseFromSequentialFile(msg, rfile.get())) {
    return Status::IOError("Unable to parse PB from path", path);
  }
  return Status::OK();
}

static void TruncateString(string* s, int max_len) {
  if (s->size() > max_len) {
    s->resize(max_len);
    s->append("<truncated>");
  }
}

void TruncateFields(Message* message, int max_len) {
  const Reflection* reflection = message->GetReflection();
  vector<const FieldDescriptor*> fields;
  reflection->ListFields(*message, &fields);
  BOOST_FOREACH(const FieldDescriptor* field, fields) {
    if (field->is_repeated()) {
      for (int i = 0; i < reflection->FieldSize(*message, field); i++) {
        switch (field->cpp_type()) {
          case FieldDescriptor::CPPTYPE_STRING: {
            const string& s_const = reflection->GetRepeatedStringReference(*message, field, i,
                                                                           NULL);
            TruncateString(const_cast<string*>(&s_const), max_len);
            break;
          }
          case FieldDescriptor::CPPTYPE_MESSAGE: {
            TruncateFields(reflection->MutableRepeatedMessage(message, field, i), max_len);
            break;
          }
          default:
            break;
        }
      }
    } else {
      switch (field->cpp_type()) {
        case FieldDescriptor::CPPTYPE_STRING: {
          const string& s_const = reflection->GetStringReference(*message, field, NULL);
          TruncateString(const_cast<string*>(&s_const), max_len);
          break;
        }
        case FieldDescriptor::CPPTYPE_MESSAGE: {
          TruncateFields(reflection->MutableMessage(message, field), max_len);
          break;
        }
        default:
          break;
      }
    }
  }
}

WritablePBContainerFile::WritablePBContainerFile(gscoped_ptr<WritableFile> writer)
  : closed_(false),
    writer_(writer.Pass()) {
}

WritablePBContainerFile::~WritablePBContainerFile() {
  WARN_NOT_OK(Close(), "Could not Close() when destroying file");
}

Status WritablePBContainerFile::Init(const char* magic) {
  DCHECK(!closed_);

  DCHECK_EQ(kPBContainerMagicLen, strlen(magic))
      << "Magic number string incorrect length";

  faststring buf;
  buf.resize(kPBContainerHeaderLen);

  // Serialize the magic.
  strings::memcpy_inlined(buf.data(), magic, kPBContainerMagicLen);
  size_t offset = kPBContainerMagicLen;

  // Serialize the version.
  InlineEncodeFixed32(buf.data() + offset, kPBContainerVersion);
  offset += sizeof(uint32_t);

  // Write the serialized buffer to the file.
  DCHECK_EQ(kPBContainerHeaderLen, offset)
    << "Serialized unexpected number of total bytes";
  RETURN_NOT_OK_PREPEND(writer_->Append(buf), "Failed to Append() header to file");

  return Status::OK();
}

Status WritablePBContainerFile::Append(const MessageLite& msg) {
  DCHECK(!closed_);

  DCHECK(msg.IsInitialized()) << InitializationErrorMessage("serialize", msg);
  int data_size = msg.ByteSize();

  faststring buf;
  uint64_t bufsize = sizeof(uint32_t) + data_size + kPBContainerChecksumLen;
  buf.resize(bufsize);

  // Serialize the data size.
  InlineEncodeFixed32(buf.data(), static_cast<uint32_t>(data_size));
  size_t offset = sizeof(uint32_t);

  // Serialize the data.
  if (PREDICT_FALSE(!msg.SerializeWithCachedSizesToArray(buf.data() + offset))) {
    return Status::IOError("Failed to serialize PB to array");
  }
  offset += data_size;

  // Calculate and serialize the checksum.
  uint32_t checksum = crc::Crc32c(buf.data(), offset);
  InlineEncodeFixed32(buf.data() + offset, checksum);
  offset += kPBContainerChecksumLen;

  // Write the serialized buffer to the file.
  DCHECK_EQ(bufsize, offset) << "Serialized unexpected number of total bytes";
  RETURN_NOT_OK_PREPEND(writer_->Append(buf), "Failed to Append() data to file");

  return Status::OK();
}

Status WritablePBContainerFile::Flush() {
  DCHECK(!closed_);

  // TODO: Flush just the dirty bytes.
  RETURN_NOT_OK_PREPEND(writer_->Flush(WritableFile::FLUSH_ASYNC), "Failed to Flush() file");

  return Status::OK();
}

Status WritablePBContainerFile::Sync() {
  DCHECK(!closed_);

  RETURN_NOT_OK_PREPEND(writer_->Sync(), "Failed to Sync() file");

  return Status::OK();
}

Status WritablePBContainerFile::Close() {
  if (!closed_) {
    closed_ = true;

    RETURN_NOT_OK_PREPEND(writer_->Close(), "Failed to Close() file");
  }

  return Status::OK();
}

ReadablePBContainerFile::ReadablePBContainerFile(gscoped_ptr<RandomAccessFile> reader)
  : offset_(0),
    reader_(reader.Pass()) {
}

ReadablePBContainerFile::~ReadablePBContainerFile() {
  WARN_NOT_OK(Close(), "Could not Close() when destroying file");
}

Status ReadablePBContainerFile::Init(const char* magic) {
  DCHECK_EQ(kPBContainerMagicLen, strlen(magic)) << "Magic number string incorrect length";

  // Read header data.
  Slice header;
  gscoped_ptr<uint8_t[]> scratch;
  RETURN_NOT_OK_PREPEND(ValidateAndRead(kPBContainerHeaderLen, EOF_NOT_OK, &header, &scratch),
                        Substitute("Could not read header for proto container file $0",
                                   reader_->ToString()));

  // Validate magic number.
  if (PREDICT_FALSE(!strings::memeq(magic, header.data(), kPBContainerMagicLen))) {
    string file_magic(reinterpret_cast<const char*>(header.data()), kPBContainerMagicLen);
    return Status::Corruption("Invalid magic number",
                              Substitute("Expected: $0, found: $1",
                                         Utf8SafeCEscape(magic),
                                         Utf8SafeCEscape(file_magic)));
  }

  // Validate container file version.
  uint32_t version = DecodeFixed32(header.data() + kPBContainerMagicLen);
  if (PREDICT_FALSE(version != kPBContainerVersion)) {
    // We only support version 1.
    return Status::NotSupported(
        Substitute("Protobuf container has version $0, we only support version $1",
                   version, kPBContainerVersion));
  }

  return Status::OK();
}

Status ReadablePBContainerFile::ReadNextPB(MessageLite* msg) {
  // Read the size from the file. EOF here is acceptable: it means we're
  // out of PB entries.
  Slice size;
  gscoped_ptr<uint8_t[]> size_scratch;
  RETURN_NOT_OK_PREPEND(ValidateAndRead(sizeof(uint32_t), EOF_OK, &size, &size_scratch),
                        Substitute("Could not read data size from proto container file $0",
                                   reader_->ToString()));
  uint32_t data_size = DecodeFixed32(size.data());

  // Read body into buffer for checksum & parsing.
  Slice body;
  gscoped_ptr<uint8_t[]> body_scratch;
  RETURN_NOT_OK_PREPEND(ValidateAndRead(data_size, EOF_NOT_OK, &body, &body_scratch),
                        Substitute("Could not read body from proto container file $0",
                                   reader_->ToString()));

  // Read checksum.
  uint32_t expected_checksum = 0;
  {
    Slice encoded_checksum;
    gscoped_ptr<uint8_t[]> encoded_checksum_scratch;
    RETURN_NOT_OK_PREPEND(ValidateAndRead(kPBContainerChecksumLen, EOF_NOT_OK,
                                          &encoded_checksum, &encoded_checksum_scratch),
                          Substitute("Could not read checksum from proto container file $0",
                                     reader_->ToString()));
    expected_checksum = DecodeFixed32(encoded_checksum.data());
  }

  // Validate CRC32C checksum.
  Crc* crc32c = crc::GetCrc32cInstance();
  uint64_t actual_checksum = 0;
  // Compute a rolling checksum over the two byte arrays (size, body).
  crc32c->Compute(size.data(), size.size(), &actual_checksum);
  crc32c->Compute(body.data(), body.size(), &actual_checksum);
  if (PREDICT_FALSE(actual_checksum != expected_checksum)) {
    return Status::Corruption(Substitute("Incorrect checksum of file $0: actually $1, expected $2",
                                         reader_->ToString(), actual_checksum, expected_checksum));
  }

  // The checksum is correct. Time to decode the body.
  if (PREDICT_FALSE(!msg->ParseFromArray(body.data(), body.size()))) {
    return Status::IOError("Unable to parse PB from path", reader_->ToString());
  }

  return Status::OK();
}

Status ReadablePBContainerFile::Close() {
  gscoped_ptr<RandomAccessFile> deleter;
  deleter.swap(reader_);
  return Status::OK();
}

Status ReadablePBContainerFile::ValidateAndRead(size_t length, EofOK eofOK,
                                                Slice* result, gscoped_ptr<uint8_t[]>* scratch) {
  // Validate the read length using the file size.
  uint64_t file_size;
  RETURN_NOT_OK(reader_->Size(&file_size));
  if (offset_ + length > file_size) {
    switch (eofOK) {
      case EOF_OK:
        return Status::EndOfFile("Reached end of file");
      case EOF_NOT_OK:
        return Status::Corruption("File size not large enough to be valid",
                                  Substitute("Proto container file $0: "
                                      "tried to read $0 bytes at offset "
                                      "$1 but file size is only $2",
                                      reader_->ToString(), length,
                                      offset_, file_size));
      default:
        LOG(FATAL) << "Unknown value for eofOK: " << eofOK;
    }
  }

  // Perform the read.
  Slice s;
  gscoped_ptr<uint8_t[]> local_scratch(new uint8_t[length]);
  RETURN_NOT_OK(reader_->Read(offset_, length, &s, local_scratch.get()));

  // Sanity check the result.
  if (PREDICT_FALSE(s.size() < length)) {
    return Status::Corruption("Unexpected short read", Substitute(
        "Proto container file $0: tried to read $1 bytes; got $2 bytes",
        reader_->ToString(), length, s.size()));
  }

  *result = s;
  scratch->swap(local_scratch);
  offset_ += s.size();
  return Status::OK();
}


Status ReadPBContainerFromPath(Env* env, const std::string& path,
                               const char* magic, MessageLite* msg) {
  gscoped_ptr<RandomAccessFile> file;
  RETURN_NOT_OK(env->NewRandomAccessFile(path, &file));

  ReadablePBContainerFile pb_file(file.Pass());
  RETURN_NOT_OK(pb_file.Init(magic));
  RETURN_NOT_OK(pb_file.ReadNextPB(msg));
  return pb_file.Close();
}

Status WritePBContainerToPath(Env* env, const std::string& path,
                              const char* magic, const MessageLite& msg,
                              SyncMode sync) {
  const string tmp_template = path + kTmpTemplateSuffix;
  string tmp_path;

  gscoped_ptr<WritableFile> file;
  RETURN_NOT_OK(env->NewTempWritableFile(WritableFileOptions(), tmp_template, &tmp_path, &file));
  env_util::ScopedFileDeleter tmp_deleter(env, tmp_path);

  WritablePBContainerFile pb_file(file.Pass());
  RETURN_NOT_OK(pb_file.Init(magic));
  RETURN_NOT_OK(pb_file.Append(msg));
  if (sync == pb_util::SYNC) {
    RETURN_NOT_OK(pb_file.Sync());
  }
  RETURN_NOT_OK(pb_file.Close());
  RETURN_NOT_OK_PREPEND(env->RenameFile(tmp_path, path),
                        "Failed to rename tmp file to " + path);
  tmp_deleter.Cancel();
  if (sync == pb_util::SYNC) {
    RETURN_NOT_OK_PREPEND(env->SyncDir(DirName(path)),
                          "Failed to SyncDir() parent of " + path);
  }
  return Status::OK();
}

} // namespace pb_util
} // namespace kudu
