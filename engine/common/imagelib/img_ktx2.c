/*
img_ktx2.c - KTX2 format load
Copyright (C) 2023 Provod

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#include "imagelib.h"
#include "xash3d_mathlib.h"
#include "ktx2.h"

static pixformat_t Image_KTX2Format( uint32_t ktx2_format ) {
	switch (ktx2_format) {
		case KTX2_FORMAT_BC5_UNORM_BLOCK: return PF_BC5_UNSIGNED;
		case KTX2_FORMAT_BC5_SNORM_BLOCK: return PF_BC5_SIGNED;
		case KTX2_FORMAT_BC6H_UFLOAT_BLOCK: return PF_BC6H_UNSIGNED;
		case KTX2_FORMAT_BC6H_SFLOAT_BLOCK: return PF_BC6H_SIGNED;
		//case KTX2_FORMAT_BC7_UNORM_BLOCK: return PF_BC7_UNSIGNED;
		//case KTX2_FORMAT_BC7_SRGB_BLOCK: return PF_BC7_SRGB;
		default: return PF_UNKNOWN;
	}
}

// FIXME this codebase has too many copies of this function:
// - ref_gl has one
// - ref_vk has one
// - ref_soft has one
// - img_dds has one
// - many more places probably have one too
// - and now img_ktx2 also has one!
static size_t ImageSizeForType( int type, int width, int height, int depth )
{
	switch( type )
	{
	case PF_DXT1: return ((( width + 3 ) / 4 ) * (( height + 3 ) / 4 ) * depth * 8 );
	case PF_DXT3:
	case PF_DXT5:
	case PF_BC6H_SIGNED:
	case PF_BC6H_UNSIGNED:
	case PF_BC7:
	case PF_BC5_UNSIGNED:
	case PF_BC5_SIGNED:
	case PF_ATI2: return ((( width + 3 ) / 4 ) * (( height + 3 ) / 4 ) * depth * 16 );
	case PF_LUMINANCE: return (width * height * depth);
	case PF_BGR_24:
	case PF_RGB_24: return (width * height * depth * 3);
	case PF_BGRA_32:
	case PF_RGBA_32: return (width * height * depth * 4);
	}

	return 0;
}

static qboolean Image_KTX2Parse( const ktx2_header_t *header, const byte *buffer, fs_offset_t filesize ) {
	const pixformat_t format = Image_KTX2Format(header->vkFormat);
	ktx2_index_t index;
	size_t total_size = 0;
	size_t max_offset = 0;
	const byte *const levels_begin = buffer + KTX2_LEVELS_OFFSET;

	// TODO add logs for these
	if (format == PF_UNKNOWN) {
		Con_DPrintf(S_ERROR "%s: unsupported KTX2 format %d\n", __FUNCTION__, header->vkFormat);
		return false;
	}

	if (header->pixelDepth > 1) {
		Con_DPrintf(S_ERROR "%s: unsupported KTX2 pixelDepth %d\n", __FUNCTION__, header->pixelDepth);
		return false;
	}

	if (header->faceCount > 1) {
		Con_DPrintf(S_ERROR "%s: unsupported KTX2 faceCount %d\n", __FUNCTION__, header->faceCount);
		return false;
	}

	if (header->layerCount > 1) {
		Con_DPrintf(S_ERROR "%s: unsupported KTX2 layerCount %d\n", __FUNCTION__, header->layerCount);
		return false;
	}

	if (header->supercompressionScheme != 0) {
		Con_DPrintf(S_ERROR "%s: unsupported KTX2 supercompressionScheme %d\n", __FUNCTION__, header->supercompressionScheme);
		return false;
	}

	if (header->levelCount * sizeof(ktx2_level_t) + KTX2_LEVELS_OFFSET > filesize) {
		Con_DPrintf(S_ERROR "%s: file abruptly ends\n", __FUNCTION__);
		return false;
	}

	memcpy(&index, buffer + KTX2_IDENTIFIER_SIZE + sizeof(ktx2_header_t), sizeof index);

	for (int mip = 0; mip < header->levelCount; ++mip) {
		const uint32_t width = Q_max( 1, ( header->pixelWidth >> mip ));
		const uint32_t height = Q_max( 1, ( header->pixelHeight >> mip ));
		const uint32_t mip_size = ImageSizeForType( format, width, height, image.depth );

		ktx2_level_t level;
		memcpy(&level, levels_begin + mip * sizeof level, sizeof level);

		if (mip_size != level.byteLength) {
			Con_DPrintf(S_ERROR "%s: mip=%d size mismatch read=%d, but computed=%d\n",
				__FUNCTION__, mip, (int)level.byteLength, mip_size);
			return false;
		}

		total_size += level.byteLength;
		max_offset = Q_max(max_offset, level.byteLength + level.byteOffset);
	}

	if (max_offset > filesize)
		return false;

	image.type = format;
	image.size = total_size;
	image.num_mips = header->levelCount;

	// FIXME format-dependent
	image.flags = IMAGE_HAS_COLOR; // | IMAGE_HAS_ALPHA

	image.rgba = Mem_Malloc( host.imagepool, image.size );
	memcpy(image.rgba, buffer, image.size);

	for (int mip = 0, cursor = 0; mip < header->levelCount; ++mip) {
		ktx2_level_t level;
		memcpy(&level, levels_begin + mip * sizeof level, sizeof level);
		memcpy(image.rgba + cursor, buffer + level.byteOffset, level.byteLength);
	}

	return true;
}

qboolean Image_LoadKTX2( const char *name, const byte *buffer, fs_offset_t filesize ) {
	ktx2_header_t header;

	if( filesize < KTX2_MINIMAL_HEADER_SIZE )
		return false;

	if ( memcmp(buffer, KTX2_IDENTIFIER, KTX2_IDENTIFIER_SIZE) != 0 ) {
		Con_DPrintf( S_ERROR "%s: (%s) has invalid identifier\n", __FUNCTION__, name );
		return false;
	}

	memcpy(&header, buffer + KTX2_IDENTIFIER_SIZE, sizeof header);

	image.width = header.pixelWidth;
	image.height = header.pixelHeight;
	image.depth = Q_max(1, header.pixelDepth);
	image.num_mips = 1;

	if (!Image_KTX2Parse(&header, buffer, filesize)) {
		// TODO something like Image_CheckFlag( IL_SUPPORTS_KTX2_RAW )?
		Con_DPrintf( S_WARN "%s: (%s) could not be converted to supported imagelib format, passing as raw KTX2 data\n", __FUNCTION__, name );
		// This is a catch-all for ref_vk, which can do this format directly and natively
		image.type = PF_KTX2_RAW;

		image.size = filesize;
		//image.encode = TODO custom encode type?

		// FIXME format-dependent
		image.flags = IMAGE_HAS_COLOR; // | IMAGE_HAS_ALPHA

		image.rgba = Mem_Malloc( host.imagepool, image.size );
		memcpy(image.rgba, buffer, image.size);
	}

	return true;
}
