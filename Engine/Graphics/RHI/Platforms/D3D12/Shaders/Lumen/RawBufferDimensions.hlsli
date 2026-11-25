uint byteSize(ByteAddressBuffer bufferObject)
{
    uint size;
    bufferObject.GetDimensions(size);
    return size;
}

uint byteSize(RWByteAddressBuffer bufferObject)
{
    uint size;
    bufferObject.GetDimensions(size);
    return size;
}
