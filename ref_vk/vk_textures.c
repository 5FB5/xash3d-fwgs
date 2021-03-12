#include "vk_textures.h"

#include "vk_common.h"
#include "vk_core.h"
#include "vk_const.h"

#include "xash3d_mathlib.h"
#include "crtlib.h"
#include "crclib.h"
#include "com_strings.h"
#include "eiface.h"

#include <memory.h>
#include <math.h>

#define TEXTURES_HASH_SIZE	(MAX_TEXTURES >> 2)

static vk_texture_t vk_textures[MAX_TEXTURES];
static vk_texture_t* vk_texturesHashTable[TEXTURES_HASH_SIZE];
static uint	vk_numTextures;
vk_textures_global_t tglob;

static void VK_CreateInternalTextures(void);

void initTextures( void )
{
	memset( vk_textures, 0, sizeof( vk_textures ));
	memset( vk_texturesHashTable, 0, sizeof( vk_texturesHashTable ));
	vk_numTextures = 0;

	// create unused 0-entry
	Q_strncpy( vk_textures->name, "*unused*", sizeof( vk_textures->name ));
	vk_textures->hashValue = COM_HashKey( vk_textures->name, TEXTURES_HASH_SIZE );
	vk_textures->nextHash = vk_texturesHashTable[vk_textures->hashValue];
	vk_texturesHashTable[vk_textures->hashValue] = vk_textures;
	vk_numTextures = 1;

	/* FIXME
	// validate cvars
	R_SetTextureParameters();
	*/

	VK_CreateInternalTextures();

	/* FIXME
	gEngine.Cmd_AddCommand( "texturelist", R_TextureList_f, "display loaded textures list" );
	*/

	tglob.all_textures = vk_core.descriptor_pool.one_texture_sets[0];
}

void destroyTextures( void )
{
	for( unsigned int i = 0; i < vk_numTextures; i++ )
		VK_FreeTexture( i );

	//memset( tr.lightmapTextures, 0, sizeof( tr.lightmapTextures ));
	memset( vk_texturesHashTable, 0, sizeof( vk_texturesHashTable ));
	memset( vk_textures, 0, sizeof( vk_textures ));
	vk_numTextures = 0;
}

vk_texture_t *findTexture(int index)
{
	ASSERT(index >= 0);
	ASSERT(index < MAX_TEXTURES);
	return vk_textures + index;
}

static vk_texture_t *Common_AllocTexture( const char *name, texFlags_t flags )
{
	vk_texture_t	*tex;
	uint		i;

	// find a free texture_t slot
	for( i = 0, tex = vk_textures; i < vk_numTextures; i++, tex++ )
		if( !tex->name[0] ) break;

	if( i == vk_numTextures )
	{
		if( vk_numTextures == MAX_TEXTURES )
			gEngine.Host_Error( "VK_AllocTexture: MAX_TEXTURES limit exceeds\n" );
		vk_numTextures++;
	}

	gEngine.Con_Printf("Total textures: %d\n", vk_numTextures);

	tex = &vk_textures[i];

	// copy initial params
	Q_strncpy( tex->name, name, sizeof( tex->name ));
	if( FBitSet( flags, TF_SKYSIDE ))
		tex->texnum = tglob.skyboxbasenum++;
	else tex->texnum = i; // texnum is used for fast acess into vk_textures array too
	tex->flags = flags;

	// add to hash table
	tex->hashValue = COM_HashKey( name, TEXTURES_HASH_SIZE );
	tex->nextHash = vk_texturesHashTable[tex->hashValue];
	vk_texturesHashTable[tex->hashValue] = tex;

	return tex;
}

static qboolean Common_CheckTexName( const char *name )
{
	int len;

	if( !COM_CheckString( name ))
		return false;

	len = Q_strlen( name );

	// because multi-layered textures can exceed name string
	if( len >= sizeof( vk_textures->name ))
	{
		gEngine.Con_Printf( S_ERROR "LoadTexture: too long name %s (%d)\n", name, len );
		return false;
	}

	return true;
}

static vk_texture_t *Common_TextureForName( const char *name )
{
	vk_texture_t	*tex;
	uint		hash;

	// find the texture in array
	hash = COM_HashKey( name, TEXTURES_HASH_SIZE );

	for( tex = vk_texturesHashTable[hash]; tex != NULL; tex = tex->nextHash )
	{
		if( !Q_stricmp( tex->name, name ))
			return tex;
	}

	return NULL;
}

static rgbdata_t *Common_FakeImage( int width, int height, int depth, int flags )
{
	static byte	data2D[1024]; // 16x16x4
	static rgbdata_t	r_image;

	// also use this for bad textures, but without alpha
	r_image.width = Q_max( 1, width );
	r_image.height = Q_max( 1, height );
	r_image.depth = Q_max( 1, depth );
	r_image.flags = flags;
	r_image.type = PF_RGBA_32;
	r_image.size = r_image.width * r_image.height * r_image.depth * 4;
	r_image.buffer = (r_image.size > sizeof( data2D )) ? NULL : data2D;
	r_image.palette = NULL;
	r_image.numMips = 1;
	r_image.encode = 0;

	if( FBitSet( r_image.flags, IMAGE_CUBEMAP ))
		r_image.size *= 6;
	memset( data2D, 0xFF, sizeof( data2D ));

	return &r_image;
}

/*
===============
GL_ProcessImage

do specified actions on pixels
===============
*/
static void VK_ProcessImage( vk_texture_t *tex, rgbdata_t *pic )
{
	float	emboss_scale = 0.0f;
	uint	img_flags = 0;

	// force upload texture as RGB or RGBA (detail textures requires this)
	if( tex->flags & TF_FORCE_COLOR ) pic->flags |= IMAGE_HAS_COLOR;
	if( pic->flags & IMAGE_HAS_ALPHA ) tex->flags |= TF_HAS_ALPHA;

	//FIXME provod: ??? tex->encode = pic->encode; // share encode method

	if( ImageDXT( pic->type ))
	{
		if( !pic->numMips )
			tex->flags |= TF_NOMIPMAP; // disable mipmapping by user request

		// clear all the unsupported flags
		tex->flags &= ~TF_KEEP_SOURCE;
	}
	else
	{
		// copy flag about luma pixels
		if( pic->flags & IMAGE_HAS_LUMA )
			tex->flags |= TF_HAS_LUMA;

		if( pic->flags & IMAGE_QUAKEPAL )
			tex->flags |= TF_QUAKEPAL;

		// create luma texture from quake texture
		if( tex->flags & TF_MAKELUMA )
		{
			img_flags |= IMAGE_MAKE_LUMA;
			tex->flags &= ~TF_MAKELUMA;
		}

		if( tex->flags & TF_ALLOW_EMBOSS )
		{
			img_flags |= IMAGE_EMBOSS;
			tex->flags &= ~TF_ALLOW_EMBOSS;
		}

		/* FIXME provod: ???
		if( !FBitSet( tex->flags, TF_IMG_UPLOADED ) && FBitSet( tex->flags, TF_KEEP_SOURCE ))
			tex->original = gEngfuncs.FS_CopyImage( pic ); // because current pic will be expanded to rgba
		*/

		// we need to expand image into RGBA buffer
		if( pic->type == PF_INDEXED_24 || pic->type == PF_INDEXED_32 )
			img_flags |= IMAGE_FORCE_RGBA;

		/* FIXME provod: ???
		// dedicated server doesn't register this variable
		if( gl_emboss_scale != NULL )
			emboss_scale = gl_emboss_scale->value;
		*/

		// processing image before uploading (force to rgba, make luma etc)
		if( pic->buffer ) gEngine.Image_Process( &pic, 0, 0, img_flags, emboss_scale );

		if( FBitSet( tex->flags, TF_LUMINANCE ))
			ClearBits( pic->flags, IMAGE_HAS_COLOR );
	}
}

static void VK_CreateInternalTextures( void )
{
	int	dx2, dy, d;
	int	x, y;
	rgbdata_t	*pic;

	// emo-texture from quake1
	pic = Common_FakeImage( 16, 16, 1, IMAGE_HAS_COLOR );

	for( y = 0; y < 16; y++ )
	{
		for( x = 0; x < 16; x++ )
		{
			if(( y < 8 ) ^ ( x < 8 ))
				((uint *)pic->buffer)[y*16+x] = 0xFFFF00FF;
			else ((uint *)pic->buffer)[y*16+x] = 0xFF000000;
		}
	}

	tglob.defaultTexture = VK_LoadTextureInternal( REF_DEFAULT_TEXTURE, pic, TF_COLORMAP );

	// particle texture from quake1
	pic = Common_FakeImage( 16, 16, 1, IMAGE_HAS_COLOR|IMAGE_HAS_ALPHA );

	for( x = 0; x < 16; x++ )
	{
		dx2 = x - 8;
		dx2 = dx2 * dx2;

		for( y = 0; y < 16; y++ )
		{
			dy = y - 8;
			d = 255 - 35 * sqrt( dx2 + dy * dy );
			pic->buffer[( y * 16 + x ) * 4 + 3] = bound( 0, d, 255 );
		}
	}

	tglob.particleTexture = VK_LoadTextureInternal( REF_PARTICLE_TEXTURE, pic, TF_CLAMP );

	// white texture
	pic = Common_FakeImage( 4, 4, 1, IMAGE_HAS_COLOR );
	for( x = 0; x < 16; x++ )
		((uint *)pic->buffer)[x] = 0xFFFFFFFF;
	tglob.whiteTexture = VK_LoadTextureInternal( REF_WHITE_TEXTURE, pic, TF_COLORMAP );

	// gray texture
	pic = Common_FakeImage( 4, 4, 1, IMAGE_HAS_COLOR );
	for( x = 0; x < 16; x++ )
		((uint *)pic->buffer)[x] = 0xFF7F7F7F;
	tglob.grayTexture = VK_LoadTextureInternal( REF_GRAY_TEXTURE, pic, TF_COLORMAP );

	// black texture
	pic = Common_FakeImage( 4, 4, 1, IMAGE_HAS_COLOR );
	for( x = 0; x < 16; x++ )
		((uint *)pic->buffer)[x] = 0xFF000000;
	tglob.blackTexture = VK_LoadTextureInternal( REF_BLACK_TEXTURE, pic, TF_COLORMAP );

	// cinematic dummy
	pic = Common_FakeImage( 640, 100, 1, IMAGE_HAS_COLOR );
	tglob.cinTexture = VK_LoadTextureInternal( "*cintexture", pic, TF_NOMIPMAP|TF_CLAMP );
}

static VkFormat VK_GetFormat(pixformat_t format)
{
	switch(format)
	{
		case PF_RGBA_32: return VK_FORMAT_R8G8B8A8_UNORM;
		default:
			gEngine.Con_Printf(S_WARN "FIXME unsupported pixformat_t %d\n", format);
			return VK_FORMAT_UNDEFINED;
	}
}

static qboolean VK_UploadTexture(vk_texture_t *tex, rgbdata_t *pic)
{
	const VkFormat format = VK_GetFormat(pic->type);
	const VkImageTiling tiling = VK_IMAGE_TILING_OPTIMAL;
	const VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

	if (!pic->buffer)
		return false;

	// 1. Create VkImage w/ usage = DST|SAMPLED, layout=UNDEFINED
	{
		VkImageCreateInfo image_create_info = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
			.imageType = VK_IMAGE_TYPE_2D,
			.extent.width = pic->width,
			.extent.height = pic->height,
			.extent.depth = 1,
			.format = format,
			.mipLevels = 1, // TODO pic->numMips,
			.arrayLayers = 1,
			.tiling = tiling,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.usage = usage,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		};
		XVK_CHECK(vkCreateImage(vk_core.device, &image_create_info, NULL, &tex->vk.image));

		if (vk_core.debug) {
			VkDebugUtilsObjectNameInfoEXT debug_name = {
				.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
				.objectHandle = (uint64_t)tex->vk.image,
				.objectType = VK_OBJECT_TYPE_IMAGE,
				.pObjectName = tex->name,
			};
			XVK_CHECK(vkSetDebugUtilsObjectNameEXT(vk_core.device, &debug_name));
		}
	}

	// 2. Alloc mem for VkImage and bind it (DEV_LOCAL)
	{
		VkMemoryRequirements memreq;
		vkGetImageMemoryRequirements(vk_core.device, tex->vk.image, &memreq);
		tex->vk.device_memory = allocateDeviceMemory(memreq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		XVK_CHECK(vkBindImageMemory(vk_core.device, tex->vk.image, tex->vk.device_memory.device_memory, tex->vk.device_memory.offset));
	}

	{
		const size_t staging_offset = 0; // TODO multiple staging buffer users params.staging->ptr
		// 5. Create/get cmdbuf for transitions
		VkCommandBufferBeginInfo beginfo = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
			.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		};

		// 	5.1 upload buf -> image:layout:DST
		// 		5.1.1 transitionToLayout(UNDEFINED -> DST)
		VkImageMemoryBarrier image_barrier = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.image = tex->vk.image,
			.srcAccessMask = 0,
			.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			.subresourceRange = (VkImageSubresourceRange) {
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.baseMipLevel = 0,
				.levelCount = 1, // TODO
				.baseArrayLayer = 0,
				.layerCount = 1,
			}};

		XVK_CHECK(vkBeginCommandBuffer(vk_core.cb, &beginfo));
		vkCmdPipelineBarrier(vk_core.cb,
				VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
				VK_PIPELINE_STAGE_TRANSFER_BIT,
				0, 0, NULL, 0, NULL, 1, &image_barrier);

		// 		5.1.2 copyBufferToImage for all mip levels
		//for (int i = 0; i < 1/* TODO params.mipmaps_count*/; ++i)
		{
			VkBufferImageCopy region = {0};
			region.bufferOffset = /*TODO mip->offset +*/ staging_offset;
			region.bufferRowLength = 0;
			region.bufferImageHeight = 0;
			region.imageSubresource = (VkImageSubresourceLayers){
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.mipLevel = 0, // TODO mip->mip_level,
				.baseArrayLayer = 0,
				.layerCount = 1,
			};
			region.imageExtent = (VkExtent3D){
				.width = pic->width, // TODO mip->width,
				.height = pic->height, // TODO mip->height,
				.depth = 1,
			};
			memcpy(((uint8_t*)vk_core.staging.mapped) + staging_offset, pic->buffer, pic->size);
			vkCmdCopyBufferToImage(vk_core.cb, vk_core.staging.buffer, tex->vk.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
		}

		// 	5.2 image:layout:DST -> image:layout:SAMPLED
		// 		5.2.1 transitionToLayout(DST -> SHADER_READ_ONLY)
		image_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		image_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		image_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		image_barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		image_barrier.subresourceRange = (VkImageSubresourceRange){
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.baseMipLevel = 0,
			.levelCount = 1, // FIXME params.mipmaps_count,
			.baseArrayLayer = 0,
			.layerCount = 1,
		};
		vkCmdPipelineBarrier(vk_core.cb,
				VK_PIPELINE_STAGE_TRANSFER_BIT,
				VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
				0, 0, NULL, 0, NULL, 1, &image_barrier);

		XVK_CHECK(vkEndCommandBuffer(vk_core.cb));
	}

	{
		VkSubmitInfo subinfo = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
		subinfo.commandBufferCount = 1;
		subinfo.pCommandBuffers = &vk_core.cb;
		XVK_CHECK(vkQueueSubmit(vk_core.queue, 1, &subinfo, VK_NULL_HANDLE));
		XVK_CHECK(vkQueueWaitIdle(vk_core.queue));
	}

	{
		VkImageViewCreateInfo ivci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
		ivci.format = format;
		ivci.image = tex->vk.image;
		ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		ivci.subresourceRange.baseMipLevel = 0;
		ivci.subresourceRange.levelCount = 1; // FIXME params.mipmaps_count;
		ivci.subresourceRange.baseArrayLayer = 0;
		ivci.subresourceRange.layerCount = 1;
		ivci.components = (VkComponentMapping){0, 0, 0, (pic->flags & IMAGE_HAS_ALPHA) ? 0 : VK_COMPONENT_SWIZZLE_ONE};
		XVK_CHECK(vkCreateImageView(vk_core.device, &ivci, NULL, &tex->vk.image_view));
	}
		if (vk_core.debug) {
			VkDebugUtilsObjectNameInfoEXT debug_name = {
				.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
				.objectHandle = (uint64_t)tex->vk.image_view,
				.objectType = VK_OBJECT_TYPE_IMAGE_VIEW,
				.pObjectName = tex->name,
			};
			XVK_CHECK(vkSetDebugUtilsObjectNameEXT(vk_core.device, &debug_name));
		}

/*
	// TODO how should we approach this:
	// - per-texture desc sets can be inconvenient if texture is used in different incompatible contexts
	// - update descriptor sets in batch?
	if (vk_core.descriptor_pool.next_free != MAX_DESC_SETS)
	{
		VkDescriptorImageInfo dii_tex = {
			.imageView = tex->vk.image_view,
			.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		};
		VkWriteDescriptorSet wds[] = { {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstBinding = 0,
			.dstArrayElement = 0,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.pImageInfo = &dii_tex,
		}};
		wds[0].dstSet = tex->vk.descriptor = vk_core.descriptor_pool.sets[vk_core.descriptor_pool.next_free++];
		vkUpdateDescriptorSets(vk_core.device, ARRAYSIZE(wds), wds, 0, NULL);
	}
	else
	{
		tex->vk.descriptor = VK_NULL_HANDLE;
	}
*/

	return true;
}

void VK_TextureUpdateDescriptorSet( void )
{
	// TODO cache this; most of the time we don't really need to update
	// the all_textures descriptor sets, because textures don't really
	// change that much
	VkDescriptorImageInfo dii[MAX_TEXTURES];
	VkWriteDescriptorSet wds[] = { {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstBinding = 0,
		.dstArrayElement = 0,
		.descriptorCount = ARRAYSIZE(dii),
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.pImageInfo = dii,
	}};

	for (int i = 0; i < MAX_TEXTURES; ++i) 
	{
		const vk_texture_t *tex = vk_textures + i;
		const qboolean exists = !!tex->name[0];
		dii[i].sampler = VK_NULL_HANDLE;
		dii[i].imageView = exists ? tex->vk.image_view : VK_NULL_HANDLE;
		dii[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	}

	wds[0].dstSet = tglob.all_textures;
	vkUpdateDescriptorSets(vk_core.device, ARRAYSIZE(wds), wds, 0, NULL);
}

///////////// Render API funcs /////////////

// Texture tools
int	VK_FindTexture( const char *name )
{
	vk_texture_t *tex;

	if( !Common_CheckTexName( name ))
		return 0;

	// see if already loaded
	if(( tex = Common_TextureForName( name )))
		return (tex - vk_textures);

	return 0;
}
const char*	VK_TextureName( unsigned int texnum )
{
	ASSERT( texnum >= 0 && texnum < MAX_TEXTURES );
	return vk_textures[texnum].name;
}

const byte*	VK_TextureData( unsigned int texnum )
{
	gEngine.Con_Printf(S_WARN "VK FIXME: %s\n", __FUNCTION__);
	// We don't store original texture data
	// TODO do we need to?
	return NULL;
}

int	VK_LoadTexture( const char *name, const byte *buf, size_t size, int flags )
{
	vk_texture_t	*tex;
	rgbdata_t		*pic;
	uint		picFlags = 0;

	if( !Common_CheckTexName( name ))
		return 0;

	// see if already loaded
	if(( tex = Common_TextureForName( name )))
		return (tex - vk_textures);

	if( FBitSet( flags, TF_NOFLIP_TGA ))
		SetBits( picFlags, IL_DONTFLIP_TGA );

	if( FBitSet( flags, TF_KEEP_SOURCE ) && !FBitSet( flags, TF_EXPAND_SOURCE ))
		SetBits( picFlags, IL_KEEP_8BIT );

	// set some image flags
	gEngine.Image_SetForceFlags( picFlags );

	pic = gEngine.FS_LoadImage( name, buf, size );
	if( !pic ) return 0; // couldn't loading image

	// allocate the new one
	tex = Common_AllocTexture( name, flags );

	// upload texture
	VK_ProcessImage( tex, pic );

	if( !VK_UploadTexture( tex, pic ))
	{
		memset( tex, 0, sizeof( vk_texture_t ));
		gEngine.FS_FreeImage( pic ); // release source texture
		return 0;
	}

	/* FIXME
	VK_ApplyTextureParams( tex ); // update texture filter, wrap etc
	*/

	tex->width = pic->width;
	tex->height = pic->height;

	gEngine.FS_FreeImage( pic ); // release source texture

	// NOTE: always return texnum as index in array or engine will stop work !!!
	return tex - vk_textures;
}

int	VK_CreateTexture( const char *name, int width, int height, const void *buffer, texFlags_t flags )
{
	gEngine.Con_Printf("VK FIXME: %s\n", __FUNCTION__);
	return 0;
}

int	VK_LoadTextureArray( const char **names, int flags )
{
	gEngine.Con_Printf("VK FIXME: %s\n", __FUNCTION__);
	return 0;
}

int	VK_CreateTextureArray( const char *name, int width, int height, int depth, const void *buffer, texFlags_t flags )
{
	gEngine.Con_Printf("VK FIXME: %s\n", __FUNCTION__);
	return 0;
}

void VK_FreeTexture( unsigned int texnum ) {
	vk_texture_t *tex;
	vk_texture_t **prev;
	vk_texture_t *cur;
	if( texnum <= 0 ) return;

	tex = vk_textures + texnum;

	ASSERT( tex != NULL );

	// already freed?
	if( !tex->vk.image ) return;

	// debug
	if( !tex->name[0] )
	{
		gEngine.Con_Printf( S_ERROR "GL_DeleteTexture: trying to free unnamed texture with index %u\n", texnum );
		return;
	}

	// remove from hash table
	prev = &vk_texturesHashTable[tex->hashValue];

	while( 1 )
	{
		cur = *prev;
		if( !cur ) break;

		if( cur == tex )
		{
			*prev = cur->nextHash;
			break;
		}
		prev = &cur->nextHash;
	}

	/*
	// release source
	if( tex->original )
		gEngfuncs.FS_FreeImage( tex->original );
	*/

	vkDestroyImageView(vk_core.device, tex->vk.image_view, NULL);
	vkDestroyImage(vk_core.device, tex->vk.image, NULL);
	freeDeviceMemory(&tex->vk.device_memory);
	memset(tex, 0, sizeof(*tex));
}

int VK_LoadTextureFromBuffer( const char *name, rgbdata_t *pic, texFlags_t flags, qboolean update )
{
	vk_texture_t	*tex;

	if( !Common_CheckTexName( name ))
		return 0;

	// see if already loaded
	if(( tex = Common_TextureForName( name )) && !update )
		return (tex - vk_textures);

	// couldn't loading image
	if( !pic ) return 0;

	if( update )
	{
		if( tex == NULL )
			gEngine.Host_Error( "VK_LoadTextureFromBuffer: couldn't find texture %s for update\n", name );
		SetBits( tex->flags, flags );
	}
	else
	{
		// allocate the new one
		tex = Common_AllocTexture( name, flags );
	}

	VK_ProcessImage( tex, pic );

	if( !VK_UploadTexture( tex, pic ))
	{
		memset( tex, 0, sizeof( vk_texture_t ));
		return 0;
	}

	/* FIXME
	VK_ApplyTextureParams( tex ); // update texture filter, wrap etc
	*/

	return (tex - vk_textures);
}
