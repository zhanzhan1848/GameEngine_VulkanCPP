template<typename T>
uint2 textureDimensions(Texture2D<T> image, uint level)
{
    uint2 size;
    uint levels;
    image.GetDimensions(level, size.x, size.y, levels);
    return size;
}

template<typename T>
uint2 textureDimensions(TextureCube<T> image, uint level)
{
    uint2 size;
    uint levels;
    image.GetDimensions(level, size.x, size.y, levels);
    return size;
}

template<typename T>
uint3 textureDimensions(Texture2DArray<T> image, uint level)
{
    uint3 size;
    uint levels;
    image.GetDimensions(level, size.x, size.y, size.z, levels);
    return size;
}

template<typename T>
uint3 textureDimensions(Texture3D<T> image, uint level)
{
    uint3 size;
    uint levels;
    image.GetDimensions(level, size.x, size.y, size.z, levels);
    return size;
}

template<typename T>
uint2 imageDimensions(RWTexture2D<T> image)
{
    uint2 size;
    image.GetDimensions(size.x, size.y);
    return size;
}

template<typename T>
uint3 imageDimensions(RWTexture3D<T> image)
{
    uint3 size;
    image.GetDimensions(size.x, size.y, size.z);
    return size;
}
