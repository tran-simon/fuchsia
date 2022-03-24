// Generated Code - DO NOT EDIT !!
// generated by 'emugen'


#include <string.h>
#include "magma_opcodes.h"

#include "magma_enc.h"


#include <vector>

#include <stdio.h>

#include "android/base/Tracing.h"

#include "EncoderDebug.h"

namespace {

void enc_unsupported()
{
	ALOGE("Function is unsupported\n");
}

magma_status_t magma_device_import_enc(void *self , magma_handle_t device_channel, magma_device_t* device_out)
{
	ENCODER_DEBUG_LOG("magma_device_import(device_channel:0x%x, device_out:%p)", device_channel, device_out);
	AEMU_SCOPED_TRACE("magma_device_import encode");

	magma_encoder_context_t *ctx = (magma_encoder_context_t *)self;
	IOStream *stream = ctx->m_stream;
	ChecksumCalculator *checksumCalculator = ctx->m_checksumCalculator;
	bool useChecksum = checksumCalculator->getVersion() > 0;

	const unsigned int __size_device_out =  sizeof(uint64_t);
	 unsigned char *ptr;
	 unsigned char *buf;
	 const size_t sizeWithoutChecksum = 8 + 4 + 0 + 1*4;
	 const size_t checksumSize = checksumCalculator->checksumByteSize();
	 const size_t totalSize = sizeWithoutChecksum + checksumSize;
	buf = stream->alloc(totalSize);
	ptr = buf;
	int tmp = OP_magma_device_import;memcpy(ptr, &tmp, 4); ptr += 4;
	memcpy(ptr, &totalSize, 4);  ptr += 4;

		memcpy(ptr, &device_channel, 4); ptr += 4;
	memcpy(ptr, &__size_device_out, 4); ptr += 4;

	if (useChecksum) checksumCalculator->addBuffer(buf, ptr-buf);
	if (useChecksum) checksumCalculator->writeChecksum(ptr, checksumSize); ptr += checksumSize;

	stream->readback(device_out, __size_device_out);
	if (useChecksum) checksumCalculator->addBuffer(device_out, __size_device_out);

	magma_status_t retval;
	stream->readback(&retval, 4);
	if (useChecksum) checksumCalculator->addBuffer(&retval, 4);
	if (useChecksum) {
		unsigned char *checksumBufPtr = NULL;
		unsigned char checksumBuf[ChecksumCalculator::kMaxChecksumSize];
		if (checksumSize > 0) checksumBufPtr = &checksumBuf[0];
		stream->readback(checksumBufPtr, checksumSize);
		if (!checksumCalculator->validate(checksumBufPtr, checksumSize)) {
			ALOGE("magma_device_import: GL communication error, please report this issue to b.android.com.\n");
			abort();
		}
	}
	return retval;
}

void magma_device_release_enc(void *self , magma_device_t device)
{
	ENCODER_DEBUG_LOG("magma_device_release(device:%lu)", device);
	AEMU_SCOPED_TRACE("magma_device_release encode");

	magma_encoder_context_t *ctx = (magma_encoder_context_t *)self;
	IOStream *stream = ctx->m_stream;
	ChecksumCalculator *checksumCalculator = ctx->m_checksumCalculator;
	bool useChecksum = checksumCalculator->getVersion() > 0;

	 unsigned char *ptr;
	 unsigned char *buf;
	 const size_t sizeWithoutChecksum = 8 + 8;
	 const size_t checksumSize = checksumCalculator->checksumByteSize();
	 const size_t totalSize = sizeWithoutChecksum + checksumSize;
	buf = stream->alloc(totalSize);
	ptr = buf;
	int tmp = OP_magma_device_release;memcpy(ptr, &tmp, 4); ptr += 4;
	memcpy(ptr, &totalSize, 4);  ptr += 4;

		memcpy(ptr, &device, 8); ptr += 8;

	if (useChecksum) checksumCalculator->addBuffer(buf, ptr-buf);
	if (useChecksum) checksumCalculator->writeChecksum(ptr, checksumSize); ptr += checksumSize;

}

magma_status_t magma_query_enc(void *self , magma_device_t device, uint64_t id, magma_handle_t* result_buffer_out, uint64_t* value_out)
{
	ENCODER_DEBUG_LOG("magma_query(device:%lu, id:%lu, result_buffer_out:%p, value_out:%p)", device, id, result_buffer_out, value_out);
	AEMU_SCOPED_TRACE("magma_query encode");

	magma_encoder_context_t *ctx = (magma_encoder_context_t *)self;
	IOStream *stream = ctx->m_stream;
	ChecksumCalculator *checksumCalculator = ctx->m_checksumCalculator;
	bool useChecksum = checksumCalculator->getVersion() > 0;

	const unsigned int __size_result_buffer_out =  sizeof(magma_handle_t);
	const unsigned int __size_value_out =  sizeof(uint64_t);
	 unsigned char *ptr;
	 unsigned char *buf;
	 const size_t sizeWithoutChecksum = 8 + 8 + 8 + 0 + 0 + 2*4;
	 const size_t checksumSize = checksumCalculator->checksumByteSize();
	 const size_t totalSize = sizeWithoutChecksum + checksumSize;
	buf = stream->alloc(totalSize);
	ptr = buf;
	int tmp = OP_magma_query;memcpy(ptr, &tmp, 4); ptr += 4;
	memcpy(ptr, &totalSize, 4);  ptr += 4;

		memcpy(ptr, &device, 8); ptr += 8;
		memcpy(ptr, &id, 8); ptr += 8;
	memcpy(ptr, &__size_result_buffer_out, 4); ptr += 4;
	memcpy(ptr, &__size_value_out, 4); ptr += 4;

	if (useChecksum) checksumCalculator->addBuffer(buf, ptr-buf);
	if (useChecksum) checksumCalculator->writeChecksum(ptr, checksumSize); ptr += checksumSize;

	stream->readback(result_buffer_out, __size_result_buffer_out);
	if (useChecksum) checksumCalculator->addBuffer(result_buffer_out, __size_result_buffer_out);
	stream->readback(value_out, __size_value_out);
	if (useChecksum) checksumCalculator->addBuffer(value_out, __size_value_out);

	magma_status_t retval;
	stream->readback(&retval, 4);
	if (useChecksum) checksumCalculator->addBuffer(&retval, 4);
	if (useChecksum) {
		unsigned char *checksumBufPtr = NULL;
		unsigned char checksumBuf[ChecksumCalculator::kMaxChecksumSize];
		if (checksumSize > 0) checksumBufPtr = &checksumBuf[0];
		stream->readback(checksumBufPtr, checksumSize);
		if (!checksumCalculator->validate(checksumBufPtr, checksumSize)) {
			ALOGE("magma_query: GL communication error, please report this issue to b.android.com.\n");
			abort();
		}
	}
	return retval;
}

magma_status_t magma_create_connection2_enc(void *self , magma_device_t device, magma_connection_t* connection_out)
{
	ENCODER_DEBUG_LOG("magma_create_connection2(device:%lu, connection_out:%p)", device, connection_out);
	AEMU_SCOPED_TRACE("magma_create_connection2 encode");

	magma_encoder_context_t *ctx = (magma_encoder_context_t *)self;
	IOStream *stream = ctx->m_stream;
	ChecksumCalculator *checksumCalculator = ctx->m_checksumCalculator;
	bool useChecksum = checksumCalculator->getVersion() > 0;

	const unsigned int __size_connection_out =  sizeof(uint64_t);
	 unsigned char *ptr;
	 unsigned char *buf;
	 const size_t sizeWithoutChecksum = 8 + 8 + 0 + 1*4;
	 const size_t checksumSize = checksumCalculator->checksumByteSize();
	 const size_t totalSize = sizeWithoutChecksum + checksumSize;
	buf = stream->alloc(totalSize);
	ptr = buf;
	int tmp = OP_magma_create_connection2;memcpy(ptr, &tmp, 4); ptr += 4;
	memcpy(ptr, &totalSize, 4);  ptr += 4;

		memcpy(ptr, &device, 8); ptr += 8;
	memcpy(ptr, &__size_connection_out, 4); ptr += 4;

	if (useChecksum) checksumCalculator->addBuffer(buf, ptr-buf);
	if (useChecksum) checksumCalculator->writeChecksum(ptr, checksumSize); ptr += checksumSize;

	stream->readback(connection_out, __size_connection_out);
	if (useChecksum) checksumCalculator->addBuffer(connection_out, __size_connection_out);

	magma_status_t retval;
	stream->readback(&retval, 4);
	if (useChecksum) checksumCalculator->addBuffer(&retval, 4);
	if (useChecksum) {
		unsigned char *checksumBufPtr = NULL;
		unsigned char checksumBuf[ChecksumCalculator::kMaxChecksumSize];
		if (checksumSize > 0) checksumBufPtr = &checksumBuf[0];
		stream->readback(checksumBufPtr, checksumSize);
		if (!checksumCalculator->validate(checksumBufPtr, checksumSize)) {
			ALOGE("magma_create_connection2: GL communication error, please report this issue to b.android.com.\n");
			abort();
		}
	}
	return retval;
}

void magma_release_connection_enc(void *self , magma_connection_t connection)
{
	ENCODER_DEBUG_LOG("magma_release_connection(connection:%lu)", connection);
	AEMU_SCOPED_TRACE("magma_release_connection encode");

	magma_encoder_context_t *ctx = (magma_encoder_context_t *)self;
	IOStream *stream = ctx->m_stream;
	ChecksumCalculator *checksumCalculator = ctx->m_checksumCalculator;
	bool useChecksum = checksumCalculator->getVersion() > 0;

	 unsigned char *ptr;
	 unsigned char *buf;
	 const size_t sizeWithoutChecksum = 8 + 8;
	 const size_t checksumSize = checksumCalculator->checksumByteSize();
	 const size_t totalSize = sizeWithoutChecksum + checksumSize;
	buf = stream->alloc(totalSize);
	ptr = buf;
	int tmp = OP_magma_release_connection;memcpy(ptr, &tmp, 4); ptr += 4;
	memcpy(ptr, &totalSize, 4);  ptr += 4;

		memcpy(ptr, &connection, 8); ptr += 8;

	if (useChecksum) checksumCalculator->addBuffer(buf, ptr-buf);
	if (useChecksum) checksumCalculator->writeChecksum(ptr, checksumSize); ptr += checksumSize;

}

magma_status_t magma_create_buffer_enc(void *self , magma_connection_t connection, uint64_t size, uint64_t* size_out, magma_buffer_t* buffer_out)
{
	ENCODER_DEBUG_LOG("magma_create_buffer(connection:%lu, size:%lu, size_out:%p, buffer_out:%p)", connection, size, size_out, buffer_out);
	AEMU_SCOPED_TRACE("magma_create_buffer encode");

	magma_encoder_context_t *ctx = (magma_encoder_context_t *)self;
	IOStream *stream = ctx->m_stream;
	ChecksumCalculator *checksumCalculator = ctx->m_checksumCalculator;
	bool useChecksum = checksumCalculator->getVersion() > 0;

	const unsigned int __size_size_out =  sizeof(uint64_t);
	const unsigned int __size_buffer_out =  sizeof(uint64_t);
	 unsigned char *ptr;
	 unsigned char *buf;
	 const size_t sizeWithoutChecksum = 8 + 8 + 8 + 0 + 0 + 2*4;
	 const size_t checksumSize = checksumCalculator->checksumByteSize();
	 const size_t totalSize = sizeWithoutChecksum + checksumSize;
	buf = stream->alloc(totalSize);
	ptr = buf;
	int tmp = OP_magma_create_buffer;memcpy(ptr, &tmp, 4); ptr += 4;
	memcpy(ptr, &totalSize, 4);  ptr += 4;

		memcpy(ptr, &connection, 8); ptr += 8;
		memcpy(ptr, &size, 8); ptr += 8;
	memcpy(ptr, &__size_size_out, 4); ptr += 4;
	memcpy(ptr, &__size_buffer_out, 4); ptr += 4;

	if (useChecksum) checksumCalculator->addBuffer(buf, ptr-buf);
	if (useChecksum) checksumCalculator->writeChecksum(ptr, checksumSize); ptr += checksumSize;

	stream->readback(size_out, __size_size_out);
	if (useChecksum) checksumCalculator->addBuffer(size_out, __size_size_out);
	stream->readback(buffer_out, __size_buffer_out);
	if (useChecksum) checksumCalculator->addBuffer(buffer_out, __size_buffer_out);

	magma_status_t retval;
	stream->readback(&retval, 4);
	if (useChecksum) checksumCalculator->addBuffer(&retval, 4);
	if (useChecksum) {
		unsigned char *checksumBufPtr = NULL;
		unsigned char checksumBuf[ChecksumCalculator::kMaxChecksumSize];
		if (checksumSize > 0) checksumBufPtr = &checksumBuf[0];
		stream->readback(checksumBufPtr, checksumSize);
		if (!checksumCalculator->validate(checksumBufPtr, checksumSize)) {
			ALOGE("magma_create_buffer: GL communication error, please report this issue to b.android.com.\n");
			abort();
		}
	}
	return retval;
}

void magma_release_buffer_enc(void *self , magma_connection_t connection, magma_buffer_t buffer)
{
	ENCODER_DEBUG_LOG("magma_release_buffer(connection:%lu, buffer:%lu)", connection, buffer);
	AEMU_SCOPED_TRACE("magma_release_buffer encode");

	magma_encoder_context_t *ctx = (magma_encoder_context_t *)self;
	IOStream *stream = ctx->m_stream;
	ChecksumCalculator *checksumCalculator = ctx->m_checksumCalculator;
	bool useChecksum = checksumCalculator->getVersion() > 0;

	 unsigned char *ptr;
	 unsigned char *buf;
	 const size_t sizeWithoutChecksum = 8 + 8 + 8;
	 const size_t checksumSize = checksumCalculator->checksumByteSize();
	 const size_t totalSize = sizeWithoutChecksum + checksumSize;
	buf = stream->alloc(totalSize);
	ptr = buf;
	int tmp = OP_magma_release_buffer;memcpy(ptr, &tmp, 4); ptr += 4;
	memcpy(ptr, &totalSize, 4);  ptr += 4;

		memcpy(ptr, &connection, 8); ptr += 8;
		memcpy(ptr, &buffer, 8); ptr += 8;

	if (useChecksum) checksumCalculator->addBuffer(buf, ptr-buf);
	if (useChecksum) checksumCalculator->writeChecksum(ptr, checksumSize); ptr += checksumSize;

}

uint64_t magma_get_buffer_id_enc(void *self , magma_buffer_t buffer)
{
	ENCODER_DEBUG_LOG("magma_get_buffer_id(buffer:%lu)", buffer);
	AEMU_SCOPED_TRACE("magma_get_buffer_id encode");

	magma_encoder_context_t *ctx = (magma_encoder_context_t *)self;
	IOStream *stream = ctx->m_stream;
	ChecksumCalculator *checksumCalculator = ctx->m_checksumCalculator;
	bool useChecksum = checksumCalculator->getVersion() > 0;

	 unsigned char *ptr;
	 unsigned char *buf;
	 const size_t sizeWithoutChecksum = 8 + 8;
	 const size_t checksumSize = checksumCalculator->checksumByteSize();
	 const size_t totalSize = sizeWithoutChecksum + checksumSize;
	buf = stream->alloc(totalSize);
	ptr = buf;
	int tmp = OP_magma_get_buffer_id;memcpy(ptr, &tmp, 4); ptr += 4;
	memcpy(ptr, &totalSize, 4);  ptr += 4;

		memcpy(ptr, &buffer, 8); ptr += 8;

	if (useChecksum) checksumCalculator->addBuffer(buf, ptr-buf);
	if (useChecksum) checksumCalculator->writeChecksum(ptr, checksumSize); ptr += checksumSize;


	uint64_t retval;
	stream->readback(&retval, 8);
	if (useChecksum) checksumCalculator->addBuffer(&retval, 8);
	if (useChecksum) {
		unsigned char *checksumBufPtr = NULL;
		unsigned char checksumBuf[ChecksumCalculator::kMaxChecksumSize];
		if (checksumSize > 0) checksumBufPtr = &checksumBuf[0];
		stream->readback(checksumBufPtr, checksumSize);
		if (!checksumCalculator->validate(checksumBufPtr, checksumSize)) {
			ALOGE("magma_get_buffer_id: GL communication error, please report this issue to b.android.com.\n");
			abort();
		}
	}
	return retval;
}

uint64_t magma_get_buffer_size_enc(void *self , magma_buffer_t buffer)
{
	ENCODER_DEBUG_LOG("magma_get_buffer_size(buffer:%lu)", buffer);
	AEMU_SCOPED_TRACE("magma_get_buffer_size encode");

	magma_encoder_context_t *ctx = (magma_encoder_context_t *)self;
	IOStream *stream = ctx->m_stream;
	ChecksumCalculator *checksumCalculator = ctx->m_checksumCalculator;
	bool useChecksum = checksumCalculator->getVersion() > 0;

	 unsigned char *ptr;
	 unsigned char *buf;
	 const size_t sizeWithoutChecksum = 8 + 8;
	 const size_t checksumSize = checksumCalculator->checksumByteSize();
	 const size_t totalSize = sizeWithoutChecksum + checksumSize;
	buf = stream->alloc(totalSize);
	ptr = buf;
	int tmp = OP_magma_get_buffer_size;memcpy(ptr, &tmp, 4); ptr += 4;
	memcpy(ptr, &totalSize, 4);  ptr += 4;

		memcpy(ptr, &buffer, 8); ptr += 8;

	if (useChecksum) checksumCalculator->addBuffer(buf, ptr-buf);
	if (useChecksum) checksumCalculator->writeChecksum(ptr, checksumSize); ptr += checksumSize;


	uint64_t retval;
	stream->readback(&retval, 8);
	if (useChecksum) checksumCalculator->addBuffer(&retval, 8);
	if (useChecksum) {
		unsigned char *checksumBufPtr = NULL;
		unsigned char checksumBuf[ChecksumCalculator::kMaxChecksumSize];
		if (checksumSize > 0) checksumBufPtr = &checksumBuf[0];
		stream->readback(checksumBufPtr, checksumSize);
		if (!checksumCalculator->validate(checksumBufPtr, checksumSize)) {
			ALOGE("magma_get_buffer_size: GL communication error, please report this issue to b.android.com.\n");
			abort();
		}
	}
	return retval;
}

magma_status_t magma_get_buffer_handle2_enc(void *self , magma_buffer_t buffer, magma_handle_t* handle_out)
{
	ENCODER_DEBUG_LOG("magma_get_buffer_handle2(buffer:%lu, handle_out:%p)", buffer, handle_out);
	AEMU_SCOPED_TRACE("magma_get_buffer_handle2 encode");

	magma_encoder_context_t *ctx = (magma_encoder_context_t *)self;
	IOStream *stream = ctx->m_stream;
	ChecksumCalculator *checksumCalculator = ctx->m_checksumCalculator;
	bool useChecksum = checksumCalculator->getVersion() > 0;

	const unsigned int __size_handle_out =  sizeof(uint64_t);
	 unsigned char *ptr;
	 unsigned char *buf;
	 const size_t sizeWithoutChecksum = 8 + 8 + 0 + 1*4;
	 const size_t checksumSize = checksumCalculator->checksumByteSize();
	 const size_t totalSize = sizeWithoutChecksum + checksumSize;
	buf = stream->alloc(totalSize);
	ptr = buf;
	int tmp = OP_magma_get_buffer_handle2;memcpy(ptr, &tmp, 4); ptr += 4;
	memcpy(ptr, &totalSize, 4);  ptr += 4;

		memcpy(ptr, &buffer, 8); ptr += 8;
	memcpy(ptr, &__size_handle_out, 4); ptr += 4;

	if (useChecksum) checksumCalculator->addBuffer(buf, ptr-buf);
	if (useChecksum) checksumCalculator->writeChecksum(ptr, checksumSize); ptr += checksumSize;

	stream->readback(handle_out, __size_handle_out);
	if (useChecksum) checksumCalculator->addBuffer(handle_out, __size_handle_out);

	magma_status_t retval;
	stream->readback(&retval, 4);
	if (useChecksum) checksumCalculator->addBuffer(&retval, 4);
	if (useChecksum) {
		unsigned char *checksumBufPtr = NULL;
		unsigned char checksumBuf[ChecksumCalculator::kMaxChecksumSize];
		if (checksumSize > 0) checksumBufPtr = &checksumBuf[0];
		stream->readback(checksumBufPtr, checksumSize);
		if (!checksumCalculator->validate(checksumBufPtr, checksumSize)) {
			ALOGE("magma_get_buffer_handle2: GL communication error, please report this issue to b.android.com.\n");
			abort();
		}
	}
	return retval;
}

magma_status_t magma_create_semaphore_enc(void *self , magma_connection_t connection, magma_semaphore_t* semaphore_out)
{
	ENCODER_DEBUG_LOG("magma_create_semaphore(connection:%lu, semaphore_out:%p)", connection, semaphore_out);
	AEMU_SCOPED_TRACE("magma_create_semaphore encode");

	magma_encoder_context_t *ctx = (magma_encoder_context_t *)self;
	IOStream *stream = ctx->m_stream;
	ChecksumCalculator *checksumCalculator = ctx->m_checksumCalculator;
	bool useChecksum = checksumCalculator->getVersion() > 0;

	const unsigned int __size_semaphore_out =  sizeof(uint64_t);
	 unsigned char *ptr;
	 unsigned char *buf;
	 const size_t sizeWithoutChecksum = 8 + 8 + 0 + 1*4;
	 const size_t checksumSize = checksumCalculator->checksumByteSize();
	 const size_t totalSize = sizeWithoutChecksum + checksumSize;
	buf = stream->alloc(totalSize);
	ptr = buf;
	int tmp = OP_magma_create_semaphore;memcpy(ptr, &tmp, 4); ptr += 4;
	memcpy(ptr, &totalSize, 4);  ptr += 4;

		memcpy(ptr, &connection, 8); ptr += 8;
	memcpy(ptr, &__size_semaphore_out, 4); ptr += 4;

	if (useChecksum) checksumCalculator->addBuffer(buf, ptr-buf);
	if (useChecksum) checksumCalculator->writeChecksum(ptr, checksumSize); ptr += checksumSize;

	stream->readback(semaphore_out, __size_semaphore_out);
	if (useChecksum) checksumCalculator->addBuffer(semaphore_out, __size_semaphore_out);

	magma_status_t retval;
	stream->readback(&retval, 4);
	if (useChecksum) checksumCalculator->addBuffer(&retval, 4);
	if (useChecksum) {
		unsigned char *checksumBufPtr = NULL;
		unsigned char checksumBuf[ChecksumCalculator::kMaxChecksumSize];
		if (checksumSize > 0) checksumBufPtr = &checksumBuf[0];
		stream->readback(checksumBufPtr, checksumSize);
		if (!checksumCalculator->validate(checksumBufPtr, checksumSize)) {
			ALOGE("magma_create_semaphore: GL communication error, please report this issue to b.android.com.\n");
			abort();
		}
	}
	return retval;
}

void magma_release_semaphore_enc(void *self , magma_connection_t connection, magma_semaphore_t semaphore)
{
	ENCODER_DEBUG_LOG("magma_release_semaphore(connection:%lu, semaphore:%lu)", connection, semaphore);
	AEMU_SCOPED_TRACE("magma_release_semaphore encode");

	magma_encoder_context_t *ctx = (magma_encoder_context_t *)self;
	IOStream *stream = ctx->m_stream;
	ChecksumCalculator *checksumCalculator = ctx->m_checksumCalculator;
	bool useChecksum = checksumCalculator->getVersion() > 0;

	 unsigned char *ptr;
	 unsigned char *buf;
	 const size_t sizeWithoutChecksum = 8 + 8 + 8;
	 const size_t checksumSize = checksumCalculator->checksumByteSize();
	 const size_t totalSize = sizeWithoutChecksum + checksumSize;
	buf = stream->alloc(totalSize);
	ptr = buf;
	int tmp = OP_magma_release_semaphore;memcpy(ptr, &tmp, 4); ptr += 4;
	memcpy(ptr, &totalSize, 4);  ptr += 4;

		memcpy(ptr, &connection, 8); ptr += 8;
		memcpy(ptr, &semaphore, 8); ptr += 8;

	if (useChecksum) checksumCalculator->addBuffer(buf, ptr-buf);
	if (useChecksum) checksumCalculator->writeChecksum(ptr, checksumSize); ptr += checksumSize;

}

uint64_t magma_get_semaphore_id_enc(void *self , magma_semaphore_t semaphore)
{
	ENCODER_DEBUG_LOG("magma_get_semaphore_id(semaphore:%lu)", semaphore);
	AEMU_SCOPED_TRACE("magma_get_semaphore_id encode");

	magma_encoder_context_t *ctx = (magma_encoder_context_t *)self;
	IOStream *stream = ctx->m_stream;
	ChecksumCalculator *checksumCalculator = ctx->m_checksumCalculator;
	bool useChecksum = checksumCalculator->getVersion() > 0;

	 unsigned char *ptr;
	 unsigned char *buf;
	 const size_t sizeWithoutChecksum = 8 + 8;
	 const size_t checksumSize = checksumCalculator->checksumByteSize();
	 const size_t totalSize = sizeWithoutChecksum + checksumSize;
	buf = stream->alloc(totalSize);
	ptr = buf;
	int tmp = OP_magma_get_semaphore_id;memcpy(ptr, &tmp, 4); ptr += 4;
	memcpy(ptr, &totalSize, 4);  ptr += 4;

		memcpy(ptr, &semaphore, 8); ptr += 8;

	if (useChecksum) checksumCalculator->addBuffer(buf, ptr-buf);
	if (useChecksum) checksumCalculator->writeChecksum(ptr, checksumSize); ptr += checksumSize;


	uint64_t retval;
	stream->readback(&retval, 8);
	if (useChecksum) checksumCalculator->addBuffer(&retval, 8);
	if (useChecksum) {
		unsigned char *checksumBufPtr = NULL;
		unsigned char checksumBuf[ChecksumCalculator::kMaxChecksumSize];
		if (checksumSize > 0) checksumBufPtr = &checksumBuf[0];
		stream->readback(checksumBufPtr, checksumSize);
		if (!checksumCalculator->validate(checksumBufPtr, checksumSize)) {
			ALOGE("magma_get_semaphore_id: GL communication error, please report this issue to b.android.com.\n");
			abort();
		}
	}
	return retval;
}

void magma_signal_semaphore_enc(void *self , magma_semaphore_t semaphore)
{
	ENCODER_DEBUG_LOG("magma_signal_semaphore(semaphore:%lu)", semaphore);
	AEMU_SCOPED_TRACE("magma_signal_semaphore encode");

	magma_encoder_context_t *ctx = (magma_encoder_context_t *)self;
	IOStream *stream = ctx->m_stream;
	ChecksumCalculator *checksumCalculator = ctx->m_checksumCalculator;
	bool useChecksum = checksumCalculator->getVersion() > 0;

	 unsigned char *ptr;
	 unsigned char *buf;
	 const size_t sizeWithoutChecksum = 8 + 8;
	 const size_t checksumSize = checksumCalculator->checksumByteSize();
	 const size_t totalSize = sizeWithoutChecksum + checksumSize;
	buf = stream->alloc(totalSize);
	ptr = buf;
	int tmp = OP_magma_signal_semaphore;memcpy(ptr, &tmp, 4); ptr += 4;
	memcpy(ptr, &totalSize, 4);  ptr += 4;

		memcpy(ptr, &semaphore, 8); ptr += 8;

	if (useChecksum) checksumCalculator->addBuffer(buf, ptr-buf);
	if (useChecksum) checksumCalculator->writeChecksum(ptr, checksumSize); ptr += checksumSize;

}

void magma_reset_semaphore_enc(void *self , magma_semaphore_t semaphore)
{
	ENCODER_DEBUG_LOG("magma_reset_semaphore(semaphore:%lu)", semaphore);
	AEMU_SCOPED_TRACE("magma_reset_semaphore encode");

	magma_encoder_context_t *ctx = (magma_encoder_context_t *)self;
	IOStream *stream = ctx->m_stream;
	ChecksumCalculator *checksumCalculator = ctx->m_checksumCalculator;
	bool useChecksum = checksumCalculator->getVersion() > 0;

	 unsigned char *ptr;
	 unsigned char *buf;
	 const size_t sizeWithoutChecksum = 8 + 8;
	 const size_t checksumSize = checksumCalculator->checksumByteSize();
	 const size_t totalSize = sizeWithoutChecksum + checksumSize;
	buf = stream->alloc(totalSize);
	ptr = buf;
	int tmp = OP_magma_reset_semaphore;memcpy(ptr, &tmp, 4); ptr += 4;
	memcpy(ptr, &totalSize, 4);  ptr += 4;

		memcpy(ptr, &semaphore, 8); ptr += 8;

	if (useChecksum) checksumCalculator->addBuffer(buf, ptr-buf);
	if (useChecksum) checksumCalculator->writeChecksum(ptr, checksumSize); ptr += checksumSize;

}

magma_status_t magma_poll_enc(void *self , magma_poll_item_t* items, uint32_t count, uint64_t timeout_ns)
{
	ENCODER_DEBUG_LOG("magma_poll(items:%p, count:%u, timeout_ns:%lu)", items, count, timeout_ns);
	AEMU_SCOPED_TRACE("magma_poll encode");

	magma_encoder_context_t *ctx = (magma_encoder_context_t *)self;
	IOStream *stream = ctx->m_stream;
	ChecksumCalculator *checksumCalculator = ctx->m_checksumCalculator;
	bool useChecksum = checksumCalculator->getVersion() > 0;

	const unsigned int __size_items =  sizeof(magma_poll_item_t) * count;
	 unsigned char *ptr;
	 unsigned char *buf;
	 const size_t sizeWithoutChecksum = 8 + __size_items + 4 + 8 + 1*4;
	 const size_t checksumSize = checksumCalculator->checksumByteSize();
	 const size_t totalSize = sizeWithoutChecksum + checksumSize;
	buf = stream->alloc(totalSize);
	ptr = buf;
	int tmp = OP_magma_poll;memcpy(ptr, &tmp, 4); ptr += 4;
	memcpy(ptr, &totalSize, 4);  ptr += 4;

	memcpy(ptr, &__size_items, 4); ptr += 4;
	memcpy(ptr, items, __size_items);ptr += __size_items;
		memcpy(ptr, &count, 4); ptr += 4;
		memcpy(ptr, &timeout_ns, 8); ptr += 8;

	if (useChecksum) checksumCalculator->addBuffer(buf, ptr-buf);
	if (useChecksum) checksumCalculator->writeChecksum(ptr, checksumSize); ptr += checksumSize;

	stream->readback(items, __size_items);
	if (useChecksum) checksumCalculator->addBuffer(items, __size_items);

	magma_status_t retval;
	stream->readback(&retval, 4);
	if (useChecksum) checksumCalculator->addBuffer(&retval, 4);
	if (useChecksum) {
		unsigned char *checksumBufPtr = NULL;
		unsigned char checksumBuf[ChecksumCalculator::kMaxChecksumSize];
		if (checksumSize > 0) checksumBufPtr = &checksumBuf[0];
		stream->readback(checksumBufPtr, checksumSize);
		if (!checksumCalculator->validate(checksumBufPtr, checksumSize)) {
			ALOGE("magma_poll: GL communication error, please report this issue to b.android.com.\n");
			abort();
		}
	}
	return retval;
}

magma_status_t magma_get_error_enc(void *self , magma_connection_t connection)
{
	ENCODER_DEBUG_LOG("magma_get_error(connection:%lu)", connection);
	AEMU_SCOPED_TRACE("magma_get_error encode");

	magma_encoder_context_t *ctx = (magma_encoder_context_t *)self;
	IOStream *stream = ctx->m_stream;
	ChecksumCalculator *checksumCalculator = ctx->m_checksumCalculator;
	bool useChecksum = checksumCalculator->getVersion() > 0;

	 unsigned char *ptr;
	 unsigned char *buf;
	 const size_t sizeWithoutChecksum = 8 + 8;
	 const size_t checksumSize = checksumCalculator->checksumByteSize();
	 const size_t totalSize = sizeWithoutChecksum + checksumSize;
	buf = stream->alloc(totalSize);
	ptr = buf;
	int tmp = OP_magma_get_error;memcpy(ptr, &tmp, 4); ptr += 4;
	memcpy(ptr, &totalSize, 4);  ptr += 4;

		memcpy(ptr, &connection, 8); ptr += 8;

	if (useChecksum) checksumCalculator->addBuffer(buf, ptr-buf);
	if (useChecksum) checksumCalculator->writeChecksum(ptr, checksumSize); ptr += checksumSize;


	magma_status_t retval;
	stream->readback(&retval, 4);
	if (useChecksum) checksumCalculator->addBuffer(&retval, 4);
	if (useChecksum) {
		unsigned char *checksumBufPtr = NULL;
		unsigned char checksumBuf[ChecksumCalculator::kMaxChecksumSize];
		if (checksumSize > 0) checksumBufPtr = &checksumBuf[0];
		stream->readback(checksumBufPtr, checksumSize);
		if (!checksumCalculator->validate(checksumBufPtr, checksumSize)) {
			ALOGE("magma_get_error: GL communication error, please report this issue to b.android.com.\n");
			abort();
		}
	}
	return retval;
}

magma_status_t magma_create_context_enc(void *self , magma_connection_t connection, uint32_t* context_id_out)
{
	ENCODER_DEBUG_LOG("magma_create_context(connection:%lu, context_id_out:%p)", connection, context_id_out);
	AEMU_SCOPED_TRACE("magma_create_context encode");

	magma_encoder_context_t *ctx = (magma_encoder_context_t *)self;
	IOStream *stream = ctx->m_stream;
	ChecksumCalculator *checksumCalculator = ctx->m_checksumCalculator;
	bool useChecksum = checksumCalculator->getVersion() > 0;

	const unsigned int __size_context_id_out =  sizeof(uint32_t);
	 unsigned char *ptr;
	 unsigned char *buf;
	 const size_t sizeWithoutChecksum = 8 + 8 + 0 + 1*4;
	 const size_t checksumSize = checksumCalculator->checksumByteSize();
	 const size_t totalSize = sizeWithoutChecksum + checksumSize;
	buf = stream->alloc(totalSize);
	ptr = buf;
	int tmp = OP_magma_create_context;memcpy(ptr, &tmp, 4); ptr += 4;
	memcpy(ptr, &totalSize, 4);  ptr += 4;

		memcpy(ptr, &connection, 8); ptr += 8;
	memcpy(ptr, &__size_context_id_out, 4); ptr += 4;

	if (useChecksum) checksumCalculator->addBuffer(buf, ptr-buf);
	if (useChecksum) checksumCalculator->writeChecksum(ptr, checksumSize); ptr += checksumSize;

	stream->readback(context_id_out, __size_context_id_out);
	if (useChecksum) checksumCalculator->addBuffer(context_id_out, __size_context_id_out);

	magma_status_t retval;
	stream->readback(&retval, 4);
	if (useChecksum) checksumCalculator->addBuffer(&retval, 4);
	if (useChecksum) {
		unsigned char *checksumBufPtr = NULL;
		unsigned char checksumBuf[ChecksumCalculator::kMaxChecksumSize];
		if (checksumSize > 0) checksumBufPtr = &checksumBuf[0];
		stream->readback(checksumBufPtr, checksumSize);
		if (!checksumCalculator->validate(checksumBufPtr, checksumSize)) {
			ALOGE("magma_create_context: GL communication error, please report this issue to b.android.com.\n");
			abort();
		}
	}
	return retval;
}

void magma_release_context_enc(void *self , magma_connection_t connection, uint32_t context_id)
{
	ENCODER_DEBUG_LOG("magma_release_context(connection:%lu, context_id:%u)", connection, context_id);
	AEMU_SCOPED_TRACE("magma_release_context encode");

	magma_encoder_context_t *ctx = (magma_encoder_context_t *)self;
	IOStream *stream = ctx->m_stream;
	ChecksumCalculator *checksumCalculator = ctx->m_checksumCalculator;
	bool useChecksum = checksumCalculator->getVersion() > 0;

	 unsigned char *ptr;
	 unsigned char *buf;
	 const size_t sizeWithoutChecksum = 8 + 8 + 4;
	 const size_t checksumSize = checksumCalculator->checksumByteSize();
	 const size_t totalSize = sizeWithoutChecksum + checksumSize;
	buf = stream->alloc(totalSize);
	ptr = buf;
	int tmp = OP_magma_release_context;memcpy(ptr, &tmp, 4); ptr += 4;
	memcpy(ptr, &totalSize, 4);  ptr += 4;

		memcpy(ptr, &connection, 8); ptr += 8;
		memcpy(ptr, &context_id, 4); ptr += 4;

	if (useChecksum) checksumCalculator->addBuffer(buf, ptr-buf);
	if (useChecksum) checksumCalculator->writeChecksum(ptr, checksumSize); ptr += checksumSize;

}

magma_status_t magma_map_buffer_gpu_enc(void *self , magma_connection_t connection, magma_buffer_t buffer, uint64_t page_offset, uint64_t page_count, uint64_t gpu_va, uint64_t map_flags)
{
	ENCODER_DEBUG_LOG("magma_map_buffer_gpu(connection:%lu, buffer:%lu, page_offset:%lu, page_count:%lu, gpu_va:%lu, map_flags:%lu)", connection, buffer, page_offset, page_count, gpu_va, map_flags);
	AEMU_SCOPED_TRACE("magma_map_buffer_gpu encode");

	magma_encoder_context_t *ctx = (magma_encoder_context_t *)self;
	IOStream *stream = ctx->m_stream;
	ChecksumCalculator *checksumCalculator = ctx->m_checksumCalculator;
	bool useChecksum = checksumCalculator->getVersion() > 0;

	 unsigned char *ptr;
	 unsigned char *buf;
	 const size_t sizeWithoutChecksum = 8 + 8 + 8 + 8 + 8 + 8 + 8;
	 const size_t checksumSize = checksumCalculator->checksumByteSize();
	 const size_t totalSize = sizeWithoutChecksum + checksumSize;
	buf = stream->alloc(totalSize);
	ptr = buf;
	int tmp = OP_magma_map_buffer_gpu;memcpy(ptr, &tmp, 4); ptr += 4;
	memcpy(ptr, &totalSize, 4);  ptr += 4;

		memcpy(ptr, &connection, 8); ptr += 8;
		memcpy(ptr, &buffer, 8); ptr += 8;
		memcpy(ptr, &page_offset, 8); ptr += 8;
		memcpy(ptr, &page_count, 8); ptr += 8;
		memcpy(ptr, &gpu_va, 8); ptr += 8;
		memcpy(ptr, &map_flags, 8); ptr += 8;

	if (useChecksum) checksumCalculator->addBuffer(buf, ptr-buf);
	if (useChecksum) checksumCalculator->writeChecksum(ptr, checksumSize); ptr += checksumSize;


	magma_status_t retval;
	stream->readback(&retval, 4);
	if (useChecksum) checksumCalculator->addBuffer(&retval, 4);
	if (useChecksum) {
		unsigned char *checksumBufPtr = NULL;
		unsigned char checksumBuf[ChecksumCalculator::kMaxChecksumSize];
		if (checksumSize > 0) checksumBufPtr = &checksumBuf[0];
		stream->readback(checksumBufPtr, checksumSize);
		if (!checksumCalculator->validate(checksumBufPtr, checksumSize)) {
			ALOGE("magma_map_buffer_gpu: GL communication error, please report this issue to b.android.com.\n");
			abort();
		}
	}
	return retval;
}

void magma_unmap_buffer_gpu_enc(void *self , magma_connection_t connection, magma_buffer_t buffer, uint64_t gpu_va)
{
	ENCODER_DEBUG_LOG("magma_unmap_buffer_gpu(connection:%lu, buffer:%lu, gpu_va:%lu)", connection, buffer, gpu_va);
	AEMU_SCOPED_TRACE("magma_unmap_buffer_gpu encode");

	magma_encoder_context_t *ctx = (magma_encoder_context_t *)self;
	IOStream *stream = ctx->m_stream;
	ChecksumCalculator *checksumCalculator = ctx->m_checksumCalculator;
	bool useChecksum = checksumCalculator->getVersion() > 0;

	 unsigned char *ptr;
	 unsigned char *buf;
	 const size_t sizeWithoutChecksum = 8 + 8 + 8 + 8;
	 const size_t checksumSize = checksumCalculator->checksumByteSize();
	 const size_t totalSize = sizeWithoutChecksum + checksumSize;
	buf = stream->alloc(totalSize);
	ptr = buf;
	int tmp = OP_magma_unmap_buffer_gpu;memcpy(ptr, &tmp, 4); ptr += 4;
	memcpy(ptr, &totalSize, 4);  ptr += 4;

		memcpy(ptr, &connection, 8); ptr += 8;
		memcpy(ptr, &buffer, 8); ptr += 8;
		memcpy(ptr, &gpu_va, 8); ptr += 8;

	if (useChecksum) checksumCalculator->addBuffer(buf, ptr-buf);
	if (useChecksum) checksumCalculator->writeChecksum(ptr, checksumSize); ptr += checksumSize;

}

}  // namespace

magma_encoder_context_t::magma_encoder_context_t(IOStream *stream, ChecksumCalculator *checksumCalculator)
{
	m_stream = stream;
	m_checksumCalculator = checksumCalculator;

	this->magma_device_import = &magma_device_import_enc;
	this->magma_device_release = &magma_device_release_enc;
	this->magma_query = &magma_query_enc;
	this->magma_create_connection2 = &magma_create_connection2_enc;
	this->magma_release_connection = &magma_release_connection_enc;
	this->magma_create_buffer = &magma_create_buffer_enc;
	this->magma_release_buffer = &magma_release_buffer_enc;
	this->magma_get_buffer_id = &magma_get_buffer_id_enc;
	this->magma_get_buffer_size = &magma_get_buffer_size_enc;
	this->magma_get_buffer_handle2 = &magma_get_buffer_handle2_enc;
	this->magma_create_semaphore = &magma_create_semaphore_enc;
	this->magma_release_semaphore = &magma_release_semaphore_enc;
	this->magma_get_semaphore_id = &magma_get_semaphore_id_enc;
	this->magma_signal_semaphore = &magma_signal_semaphore_enc;
	this->magma_reset_semaphore = &magma_reset_semaphore_enc;
	this->magma_poll = &magma_poll_enc;
	this->magma_get_error = &magma_get_error_enc;
	this->magma_create_context = &magma_create_context_enc;
	this->magma_release_context = &magma_release_context_enc;
	this->magma_map_buffer_gpu = &magma_map_buffer_gpu_enc;
	this->magma_unmap_buffer_gpu = &magma_unmap_buffer_gpu_enc;
}

