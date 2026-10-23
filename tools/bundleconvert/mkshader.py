"""Builds Unity SerializedFiles containing real Shader assets, for testing
VivifySerializedFile.cpp against a structure Unity would actually produce.

The point of these fixtures is that the parser is walking a genuine type tree --
flat depth-ordered nodes, a local string buffer, vector-wrapping-Array field
layout, alignment flags -- rather than a hardcoded guess at Unity 2021.3's
Shader layout.
"""
import struct

# ShaderCompilerPlatform
D3D11 = 4
GLES3PLUS = 9
VULKAN = 18
METAL = 14

# ShaderGpuProgramType
GLES3 = 4
DX11_VERTEX_SM50 = 16
DX11_PIXEL_SM50 = 18


# Where Unity's common string table (CommonString.h) holds the strings a Mesh
# type tree uses. Unity names a node through this table, with the offset's top
# bit set, whenever the string is in it; these are the offsets real 2019.4.28f1
# and 2021.3.16f1 Mesh trees carry, the same as the converter's kCommonStrings.
UNITY_COMMON_STRINGS = {
    "AABB": 0, "Array": 49, "Base": 55, "bool": 76, "char": 81, "data": 106, "float": 161, "int": 222,
    "Matrix4x4f": 245, "m_Name": 427, "size": 795, "string": 840, "TypelessData": 894, "UInt64": 921,
    "UInt8": 928, "unsigned int": 934, "vector": 981, "Vector3f": 997,
}


class TypeTree:
    """Flat, depth-ordered type tree with a local string buffer. With common
    given ({string: offset}), a string found there is named through Unity's
    common table instead, as Unity writes it."""

    def __init__(self, common=None):
        self.nodes = []          # (version, level, typeFlags, typeStr, nameStr, byteSize, index, metaFlag)
        self.strings = bytearray()
        self._offsets = {}
        self._common = common or {}

    def _str(self, text: str) -> int:
        if text in self._common:
            return 0x80000000 | self._common[text]
        if text in self._offsets:
            return self._offsets[text]
        offset = len(self.strings)
        self.strings += text.encode() + b"\0"
        self._offsets[text] = offset
        return offset

    def add(self, level, type_name, field_name, byte_size, *, is_array=False, align=False):
        self.nodes.append((
            1, level, 1 if is_array else 0,
            self._str(type_name), self._str(field_name),
            byte_size & 0xFFFFFFFF, len(self.nodes), 0x4000 if align else 0,
        ))
        return self

    def encode(self, sf_version: int) -> bytes:
        out = bytearray()
        out += struct.pack('<ii', len(self.nodes), len(self.strings))
        for (ver, level, flags, tstr, nstr, size, index, meta) in self.nodes:
            out += struct.pack('<HBBIIIII', ver, level, flags, tstr, nstr, size, index, meta)
            if sf_version >= 19:
                out += struct.pack('<Q', 0)   # ref type hash
        out += bytes(self.strings)
        return bytes(out)


def shader_type_tree(sf_version: int) -> TypeTree:
    """Mirrors the shape of UnityEngine.Shader: m_ParsedForm (with a string
    m_Name), then the platforms vector, then the three nested length tables and
    the compressed blob.

    offsets/compressedLengths/decompressedLengths are vector<vector<uint>> as
    Unity 2019.3+ writes them -- a vector node wrapping an Array node whose
    element is itself a vector node wrapping an Array of uint. That double
    wrapping is exactly what the parser has to traverse without hardcoding, so
    the fixture reproduces it rather than flattening it."""
    t = TypeTree()
    t.add(0, "Shader", "Base", -1)
    t.add(1, "SerializedShader", "m_ParsedForm", -1)
    t.add(2, "string", "m_Name", -1, align=True)
    t.add(3, "Array", "Array", -1, is_array=True, align=True)
    t.add(4, "int", "size", 4)
    t.add(4, "char", "data", 1)
    t.add(2, "unsigned int", "m_CustomEditorNameLen", 4)
    t.add(1, "vector", "platforms", -1)
    t.add(2, "Array", "Array", -1, is_array=True, align=True)
    t.add(3, "int", "size", 4)
    t.add(3, "unsigned int", "data", 4)
    for field in ("offsets", "compressedLengths", "decompressedLengths"):
        t.add(1, "vector", field, -1)
        t.add(2, "Array", "Array", -1, is_array=True, align=True)
        t.add(3, "int", "size", 4)
        t.add(3, "vector", "data", -1)
        t.add(4, "Array", "Array", -1, is_array=True, align=True)
        t.add(5, "int", "size", 4)
        t.add(5, "unsigned int", "data", 4)
    t.add(1, "vector", "compressedBlob", -1)
    t.add(2, "Array", "Array", -1, is_array=True, align=True)
    t.add(3, "int", "size", 4)
    t.add(3, "UInt8", "data", 1)
    return t


# --- LZ4 block format ------------------------------------------------------
#
# Only literal-run blocks are emitted. A block made entirely of literals is
# valid LZ4 -- the format does not require any matches -- and it keeps the
# fixtures readable, since the decompressed bytes appear verbatim in the file.
# lz4_block_with_match below covers the match path the literal case never
# reaches.

def lz4_literals(payload: bytes) -> bytes:
    """One all-literals LZ4 block. This is the last (and only) sequence, so it
    ends after its literals with no offset following."""
    out = bytearray()
    n = len(payload)
    token = (15 if n >= 15 else n) << 4
    out.append(token)
    if n >= 15:
        rest = n - 15
        while rest >= 255:
            out.append(255)
            rest -= 255
        out.append(rest)
    out += payload
    return bytes(out)


def lz4_block_with_match() -> tuple:
    """A hand-built block exercising the match-copy path: four literals, a
    back-reference that repeats them, then a final literal sequence.

    Returns (block, expected_plaintext)."""
    block = bytearray()
    block.append(0x40)            # 4 literals, match length 4 + 0
    block += b"ABCD"
    block += (4).to_bytes(2, "little")   # match offset 4
    block.append(0x10)            # final sequence: 1 literal, no match follows
    block += b"E"
    return bytes(block), b"ABCDABCDE"


def sub_program(program_type: int, code: bytes, *, blob_version=202012090,
                keywords=(), trailing=b"") -> bytes:
    """One compiled sub-program, in the layout Unity writes: format version,
    ShaderGpuProgramType, three statistics ints, a fourth from 2016.08, then the
    aligned keyword strings and the program byte array.

    trailing is what 2018.06 - 2020.12 blobs carry after the code (source map,
    bind channels, parameter tables); Unity aligns to four bytes before it."""
    out = bytearray()
    out += struct.pack('<ii', blob_version, program_type)
    out += bytes(12)                                  # statistics
    if blob_version >= 201608170:
        out += bytes(4)
    out += struct.pack('<i', len(keywords))
    for keyword in keywords:
        encoded = keyword.encode()
        out += struct.pack('<i', len(encoded)) + encoded
        _align4(out)
    if 201806140 <= blob_version < 202012090:
        out += struct.pack('<i', 0)                   # local keyword table
    out += struct.pack('<i', len(code)) + code
    if trailing:
        _align4(out)
        out += trailing
    return bytes(out)


def program_blob(programs, *, entry_size=8) -> bytes:
    """The container Unity puts inside one decompressed sub-blob: a count, then
    an [offset, length] table, then the sub-programs it points at."""
    header = 4 + len(programs) * entry_size
    body = bytearray()
    table = []
    for program in programs:
        table.append((header + len(body), len(program)))
        body += program
    out = bytearray()
    out += struct.pack('<I', len(programs))
    for (offset, length) in table:
        out += struct.pack('<II', offset, length)
        if entry_size == 12:
            out += struct.pack('<I', 0)
    out += body
    return bytes(out)


def segmented_chunks(entries, *, entry_size=12):
    """One platform group in Unity 2019.3+'s layout: a single entry table at the
    start of chunk 0 naming [offset, length, segment] per entry, and each
    entry's bytes in the chunk its segment names (offsets relative to that
    chunk). entries: list of (bytes, segment). Returns the decompressed chunks,
    ready for build_program_store."""
    segments = max(seg for _, seg in entries) + 1
    header = 4 + len(entries) * entry_size
    data = [bytearray() for _ in range(segments)]
    table = bytearray(struct.pack('<I', len(entries)))
    for body, seg in entries:
        offset = (header if seg == 0 else 0) + len(data[seg])
        table += struct.pack('<II', offset, len(body))
        if entry_size == 12:
            table += struct.pack('<I', seg)
        data[seg] += body
    return [bytes(table + data[0])] + [bytes(d) for d in data[1:]]


def _align4(buf: bytearray):
    while len(buf) % 4:
        buf.append(0)


def build_program_store(platform_blobs):
    """Lays out the compressed program store the way Unity does.

    platform_blobs is one list of decompressed sub-blobs per platform, in the
    same order as the shader's platforms array. Returns
    (offsets, compressed_lengths, decompressed_lengths, compressed_blob), where
    the three tables are nested one group per platform."""
    offsets, compressed_lengths, decompressed_lengths = [], [], []
    blob = bytearray()
    for sub_blobs in platform_blobs:
        group_offsets, group_compressed, group_decompressed = [], [], []
        for plain in sub_blobs:
            packed = lz4_literals(plain)
            group_offsets.append(len(blob))
            group_compressed.append(len(packed))
            group_decompressed.append(len(plain))
            blob += packed
        offsets.append(group_offsets)
        compressed_lengths.append(group_compressed)
        decompressed_lengths.append(group_decompressed)
    return offsets, compressed_lengths, decompressed_lengths, bytes(blob)


def shader_object(name: str, platforms, blob=b"\x01\x02\x03", *,
                  platform_blobs=None) -> bytes:
    """Serializes one Shader object matching shader_type_tree's layout.

    With platform_blobs given, a real program store is built and `blob` is
    ignored; without it the shader carries the same opaque three bytes it always
    has, which is what the platform-only tests want."""
    if platform_blobs is not None:
        offsets, compressed, decompressed, blob = build_program_store(platform_blobs)
    else:
        offsets = compressed = decompressed = [[] for _ in platforms]

    out = bytearray()
    encoded = name.encode()
    out += struct.pack('<i', len(encoded)) + encoded
    _align4(out)                                    # m_Name align
    out += struct.pack('<I', 0)                     # m_CustomEditorNameLen
    out += struct.pack('<i', len(platforms))
    for p in platforms:
        out += struct.pack('<I', p)
    _align4(out)                                    # platforms Array align

    for table in (offsets, compressed, decompressed):
        out += struct.pack('<i', len(table))
        for group in table:
            out += struct.pack('<i', len(group))
            for value in group:
                out += struct.pack('<I', value)
            _align4(out)                            # inner Array align
        _align4(out)                                # outer Array align

    out += struct.pack('<i', len(blob)) + blob
    _align4(out)                                    # blob Array align
    return bytes(out)


def texture_type_tree(sf_version: int) -> TypeTree:
    """Mirrors the shape of UnityEngine.Texture2D as far as the converter reads
    it: a string name, the size/format ints, the m_IsReadable bool, the
    StreamingInfo struct and the inline image data.

    Field order and presence differ between Unity versions, which is exactly why
    the parser finds every one of them by the name the tree gives it. A fixture
    that matched the parser's assumptions positionally would prove nothing.
    """
    t = TypeTree()
    t.add(0, "Texture2D", "Base", -1)
    t.add(1, "string", "m_Name", -1, align=True)
    t.add(2, "Array", "Array", -1, is_array=True)
    t.add(3, "int", "size", 4)
    t.add(3, "char", "data", 1)
    t.add(1, "int", "m_Width", 4)
    t.add(1, "int", "m_Height", 4)
    t.add(1, "int", "m_CompleteImageSize", 4)
    t.add(1, "int", "m_TextureFormat", 4)
    t.add(1, "int", "m_MipCount", 4)
    t.add(1, "bool", "m_IsReadable", 1)
    t.add(1, "bool", "m_IsPreProcessed", 1, align=True)
    t.add(1, "StreamingInfo", "m_StreamData", -1)
    t.add(2, "unsigned int", "offset", 4)
    t.add(2, "unsigned int", "size", 4)
    t.add(2, "string", "path", -1, align=True)
    t.add(3, "Array", "Array", -1, is_array=True)
    t.add(4, "int", "size", 4)
    t.add(4, "char", "data", 1)
    t.add(1, "TypelessData", "image data", -1, is_array=True)
    t.add(2, "int", "size", 4)
    t.add(2, "UInt8", "data", 1)
    return t


def texture_object(name="tex", *, width=4, height=4, texture_format=10, mip_count=1,
                   is_readable=False, image_data=b"", stream_size=0, stream_path=""):
    """One serialized Texture2D body, laid out to match texture_type_tree."""
    body = bytearray()

    def put_string(text: bytes):
        body.extend(struct.pack('<i', len(text)))
        body.extend(text)
        _align4(body)

    put_string(name.encode())
    body.extend(struct.pack('<iiiii', width, height,
                            len(image_data) or stream_size, texture_format, mip_count))
    body.append(1 if is_readable else 0)
    body.append(0)                                   # m_IsPreProcessed
    _align4(body)
    body.extend(struct.pack('<II', 0, stream_size))  # StreamingInfo offset, size
    put_string(stream_path.encode())
    body.extend(struct.pack('<i', len(image_data)))
    body.extend(image_data)
    return bytes(body)


def mesh_type_tree(offset64=False, stream_data=True) -> TypeTree:
    """UnityEngine.Mesh's type tree as a real 743 Aether bundle has it (Unity
    2019.4.28f1): every node in order, with the real types, byte sizes and ALIGN
    flags, nested structures included, so the converter walks what it walks in
    a real bundle and UnityPy reads the fixture to its last byte. offset64
    gives StreamingInfo the UInt64 offset it has from 2020.1 (a real
    2021.3.16f1 tree differs from this one only there). Strings come from
    Unity's common table where Unity takes them from it. stream_data=False
    leaves m_StreamData out, as Unity did before 2018.3."""
    t = TypeTree(common=UNITY_COMMON_STRINGS)

    def string(level, name):
        t.add(level, "string", name, -1)
        t.add(level + 1, "Array", "Array", -1, is_array=True, align=True)
        t.add(level + 2, "int", "size", 4)
        t.add(level + 2, "char", "data", 1)

    def vector(level, name, element, element_size, element_fields=None):
        t.add(level, "vector", name, -1)
        t.add(level + 1, "Array", "Array", -1, is_array=True, align=True)
        t.add(level + 2, "int", "size", 4)
        t.add(level + 2, element, "data", element_size)
        if element_fields:
            element_fields(level + 3)

    def vector3f(level, name):
        t.add(level, "Vector3f", name, 12)
        for axis in "xyz":
            t.add(level + 1, "float", axis, 4)

    def aabb(level, name):
        t.add(level, "AABB", name, 24)
        vector3f(level + 1, "m_Center")
        vector3f(level + 1, "m_Extent")

    def sub_mesh(level):
        for field, kind in (("firstByte", "unsigned int"), ("indexCount", "unsigned int"), ("topology", "int"),
                            ("baseVertex", "unsigned int"), ("firstVertex", "unsigned int"),
                            ("vertexCount", "unsigned int")):
            t.add(level, kind, field, 4)
        aabb(level, "localAABB")

    def blend_shape_vertex(level):
        for field in ("vertex", "normal", "tangent"):
            vector3f(level, field)
        t.add(level, "unsigned int", "index", 4)

    def mesh_blend_shape(level):
        t.add(level, "unsigned int", "firstVertex", 4)
        t.add(level, "unsigned int", "vertexCount", 4)
        t.add(level, "bool", "hasNormals", 1)
        t.add(level, "bool", "hasTangents", 1, align=True)

    def mesh_blend_shape_channel(level):
        string(level, "name")
        t.add(level, "unsigned int", "nameHash", 4)
        t.add(level, "int", "frameIndex", 4)
        t.add(level, "int", "frameCount", 4)

    def matrix(level):
        for row in range(4):
            for column in range(4):
                t.add(level, "float", f"e{row}{column}", 4)

    def min_max_aabb(level):
        vector3f(level, "m_Min")
        vector3f(level, "m_Max")

    def channel_info(level):
        for field in ("stream", "offset", "format", "dimension"):
            t.add(level, "UInt8", field, 1)

    def packed_bit_vector(level, name, with_range):
        t.add(level, "PackedBitVector", name, -1)
        t.add(level + 1, "unsigned int", "m_NumItems", 4)
        if with_range:
            t.add(level + 1, "float", "m_Range", 4)
            t.add(level + 1, "float", "m_Start", 4)
        vector(level + 1, "m_Data", "UInt8", 1)
        t.add(level + 1, "UInt8", "m_BitSize", 1, align=True)

    t.add(0, "Mesh", "Base", -1)
    string(1, "m_Name")
    vector(1, "m_SubMeshes", "SubMesh", 48, sub_mesh)
    t.add(1, "BlendShapeData", "m_Shapes", -1)
    vector(2, "vertices", "BlendShapeVertex", 40, blend_shape_vertex)
    vector(2, "shapes", "MeshBlendShape", 10, mesh_blend_shape)
    vector(2, "channels", "MeshBlendShapeChannel", -1, mesh_blend_shape_channel)
    vector(2, "fullWeights", "float", 4)
    vector(1, "m_BindPose", "Matrix4x4f", 64, matrix)
    vector(1, "m_BoneNameHashes", "unsigned int", 4)
    t.add(1, "unsigned int", "m_RootBoneNameHash", 4)
    vector(1, "m_BonesAABB", "MinMaxAABB", 24, min_max_aabb)
    t.add(1, "VariableBoneCountWeights", "m_VariableBoneCountWeights", -1)
    vector(2, "m_Data", "unsigned int", 4)
    t.add(1, "UInt8", "m_MeshCompression", 1)
    t.add(1, "bool", "m_IsReadable", 1)
    t.add(1, "bool", "m_KeepVertices", 1)
    t.add(1, "bool", "m_KeepIndices", 1, align=True)
    t.add(1, "int", "m_IndexFormat", 4)
    vector(1, "m_IndexBuffer", "UInt8", 1)
    t.add(1, "VertexData", "m_VertexData", -1, align=True)
    t.add(2, "unsigned int", "m_VertexCount", 4)
    vector(2, "m_Channels", "ChannelInfo", 4, channel_info)
    t.add(2, "TypelessData", "m_DataSize", -1, is_array=True, align=True)
    t.add(3, "int", "size", 4)
    t.add(3, "UInt8", "data", 1)
    t.add(1, "CompressedMesh", "m_CompressedMesh", -1)
    for name, with_range in MESH_PACKED_VECTORS:
        packed_bit_vector(2, name, with_range)
    t.add(2, "unsigned int", "m_UVInfo", 4)
    aabb(1, "m_LocalAABB")
    t.add(1, "int", "m_MeshUsageFlags", 4)
    vector(1, "m_BakedConvexCollisionMesh", "UInt8", 1)
    vector(1, "m_BakedTriangleCollisionMesh", "UInt8", 1)
    t.add(1, "float", "m_MeshMetrics[0]", 4)
    t.add(1, "float", "m_MeshMetrics[1]", 4, align=True)
    if stream_data:
        t.add(1, "StreamingInfo", "m_StreamData", -1)
        t.add(2, "UInt64" if offset64 else "unsigned int", "offset", 8 if offset64 else 4)
        t.add(2, "unsigned int", "size", 4)
        string(2, "path")
    return t


# CompressedMesh's PackedBitVectors, in order, and whether each carries
# m_Range and m_Start.
MESH_PACKED_VECTORS = (("m_Vertices", True), ("m_UV", True), ("m_Normals", True), ("m_Tangents", True),
                       ("m_Weights", False), ("m_NormalSigns", False), ("m_TangentSigns", False),
                       ("m_FloatColors", True), ("m_BoneIndices", False), ("m_Triangles", False))


def mesh_object(name="mesh", *, is_readable=False, keep_vertices=False, keep_indices=False, compression=0,
                vertex_count=3, inline_data=None, stream_offset=0, stream_size=0, stream_path="",
                offset64=False, truncate_before_stream=False, readable_byte=None, path_length=None):
    """One serialized Mesh body, laid out to match mesh_type_tree: one submesh,
    no blend shapes or bones, one position channel, an empty compressed mesh.
    A streamed mesh is what Unity writes for one it uploads asynchronously: no
    inline vertex bytes, and m_StreamData naming the .resS range. For corrupt
    bodies: readable_byte is written as m_IsReadable instead of the bool, and
    path_length as the stream path's length field instead of its real length."""
    body = bytearray()

    def put_string(text: bytes):
        body.extend(struct.pack('<i', len(text)))
        body.extend(text)
        _align4(body)

    put_string(name.encode())
    body.extend(struct.pack('<i', 1))                 # m_SubMeshes: one SubMesh (48 bytes)
    body.extend(struct.pack('<IIiIII', 0, 3, 0, 0, 0, vertex_count))  # firstByte .. vertexCount
    body.extend(bytes(24))                            # its localAABB
    body.extend(struct.pack('<iiii', 0, 0, 0, 0))     # m_Shapes' four empty vectors
    body.extend(struct.pack('<iiIii', 0, 0, 0, 0, 0))  # bind poses, bone hashes, root hash, bone AABBs, weights
    readable = (1 if is_readable else 0) if readable_byte is None else readable_byte
    body.extend(bytes([compression, readable, 1 if keep_vertices else 0, 1 if keep_indices else 0]))
    _align4(body)
    body.extend(struct.pack('<i', 0))                 # m_IndexFormat: UInt16
    indices = struct.pack('<HHH', 0, 1, 2)
    body.extend(struct.pack('<i', len(indices)))
    body.extend(indices)
    _align4(body)
    if inline_data is None:
        inline_data = b"" if (stream_size or stream_path) else bytes(12 * vertex_count)
    body.extend(struct.pack('<I', vertex_count))      # m_VertexCount
    body.extend(struct.pack('<iBBBB', 1, 0, 0, 0, 3))  # m_Channels: position, stream 0, float, 3 wide
    body.extend(struct.pack('<i', len(inline_data)))  # m_DataSize
    body.extend(inline_data)
    _align4(body)
    for _name, with_range in MESH_PACKED_VECTORS:     # m_CompressedMesh, empty
        body.extend(struct.pack('<I', 0))             # m_NumItems
        if with_range:
            body.extend(struct.pack('<ff', 0.0, 0.0))  # m_Range, m_Start
        body.extend(struct.pack('<i', 0))             # m_Data
        body.extend(b"\0")                            # m_BitSize
        _align4(body)
    body.extend(struct.pack('<I', 0))                 # m_UVInfo
    body.extend(bytes(24))                            # m_LocalAABB
    body.extend(struct.pack('<iii', 0, 0, 0))         # usage flags, two empty collision meshes
    body.extend(struct.pack('<ff', 0.0, 0.0))         # m_MeshMetrics
    if truncate_before_stream:
        return bytes(body)
    body.extend(struct.pack('<Q' if offset64 else '<I', stream_offset))
    body.extend(struct.pack('<I', stream_size))
    at = len(body)
    put_string(stream_path.encode())
    if path_length is not None:
        body[at:at + 4] = struct.pack('<I', path_length)
    return bytes(body)


def mesh_readable_offset(name: str) -> int:
    """Where mesh_object(name)'s m_IsReadable byte sits, counted from the start
    of its m_Name length field."""
    name_field = 4 + len(name.encode())
    name_field += (-name_field) % 4
    return name_field + 4 + 48 + 16 + 20 + 1


def serialized_file_with_shaders(shaders, *, sf_version=21, target=19,
                                 unity="2021.3.16f1", enable_type_tree=True,
                                 extra_class_id=None, textures=None, shader_tree=None, meshes=None,
                                 mesh_offset64=False, mesh_stream_data=True):
    """shaders: list of (name, [platform, ...]), or of already-serialized Shader
    bodies when shader_tree gives the type tree they were written against (see
    mkshader2021). textures: list of kwargs for texture_object. Returns the
    SerializedFile bytes."""
    textures = list(textures or [])
    meshes = list(meshes or [])
    tree = shader_tree if shader_tree is not None else shader_type_tree(sf_version)

    types = bytearray()
    type_count = 1 + (1 if textures else 0) + (1 if extra_class_id is not None else 0) + (1 if meshes else 0)
    types += struct.pack('<i', type_count)

    def emit_type(class_id, with_tree):
        buf = bytearray()
        buf += struct.pack('<i', class_id)
        if sf_version >= 16:
            buf += b'\x00'                          # isStrippedType
        if sf_version >= 17:
            buf += struct.pack('<h', -1)            # scriptTypeIndex
        if sf_version >= 13:
            if (sf_version < 16 and class_id < 0) or (sf_version >= 16 and class_id == 114):
                buf += bytes(16)
            buf += bytes(16)                        # old type hash
        if enable_type_tree:
            buf += with_tree.encode(sf_version) if with_tree else TypeTree().encode(sf_version)
            if sf_version >= 21:
                buf += struct.pack('<i', 0)         # type dependencies
        return bytes(buf)

    types += emit_type(48, tree)
    texture_type_index = None
    if textures:
        texture_type_index = 1
        types += emit_type(28, texture_type_tree(sf_version))
    mesh_type_index = None
    if meshes:
        mesh_type_index = 1 + (1 if textures else 0)
        types += emit_type(43, mesh_type_tree(offset64=mesh_offset64, stream_data=mesh_stream_data))
    extra_type_index = None
    if extra_class_id is not None:
        extra_type_index = 1 + (1 if textures else 0) + (1 if meshes else 0)
        other = TypeTree()
        other.add(0, "Mesh", "Base", -1)
        other.add(1, "unsigned int", "m_Dummy", 4)
        types += emit_type(extra_class_id, other)

    # A fixture entry is (name, platforms) or (name, platforms, platform_blobs).
    bodies = [bytes(entry) if isinstance(entry, (bytes, bytearray)) else
              shader_object(entry[0], entry[1], platform_blobs=entry[2] if len(entry) > 2 else None)
              for entry in shaders]
    type_indices = [0] * len(bodies)
    for spec in textures:
        bodies.append(texture_object(**spec))
        type_indices.append(texture_type_index)
    for spec in meshes:
        if not mesh_stream_data:
            spec = dict(spec, truncate_before_stream=True)
        bodies.append(mesh_object(offset64=mesh_offset64, **spec))
        type_indices.append(mesh_type_index)
    if extra_class_id is not None:
        bodies.append(struct.pack('<I', 0xDEADBEEF))
        type_indices.append(extra_type_index)

    cursor = 0
    placements = []
    for body in bodies:
        while cursor % 8:
            cursor += 1
        placements.append((cursor, len(body)))
        cursor += len(body)

    # The metadata is assembled in one buffer because the object table's 4-byte
    # alignment is measured from the start of the *file*, not from the start of
    # the table -- getting that wrong silently shifts every object entry.
    head_len = 48 if sf_version >= 22 else 20
    meta = bytearray()
    meta += unity.encode() + b"\0"
    meta += struct.pack('<i', target)
    meta += b'\x01' if enable_type_tree else b'\x00'
    meta += types
    meta += struct.pack('<i', len(bodies))
    for i, (start, size) in enumerate(placements):
        while (head_len + len(meta)) % 4:
            meta.append(0)
        meta += struct.pack('<q', i + 1)             # pathID
        if sf_version >= 22:
            meta += struct.pack('<q', start)
        else:
            meta += struct.pack('<i', start)
        meta += struct.pack('<I', size)
        meta += struct.pack('<i', type_indices[i])
    meta += struct.pack('<i', 0)                     # script types
    meta += struct.pack('<i', 0)                     # externals
    meta += struct.pack('<i', 0)                     # ref types
    meta += b'\0'                                    # user information

    metadata_size = len(meta)
    data_offset = head_len + metadata_size
    while data_offset % 16:
        data_offset += 1

    payload = bytearray()
    for (start, size), body in zip(placements, bodies):
        while len(payload) < start:
            payload.append(0)
        payload += body

    total = data_offset + len(payload)

    body = bytearray()
    if sf_version >= 22:
        body += struct.pack('>IIII', 0, 0, sf_version, 0)
        body += b'\x00\x00\x00\x00'
        body += struct.pack('>I', metadata_size)
        body += struct.pack('>Q', total)
        body += struct.pack('>Q', data_offset)
        body += struct.pack('>Q', 0)
    else:
        body += struct.pack('>I', metadata_size)
        body += struct.pack('>I', total)
        body += struct.pack('>I', sf_version)
        body += struct.pack('>I', data_offset)
        body += b'\x00\x00\x00\x00'
    body += meta
    while len(body) < data_offset:
        body.append(0)
    body += payload
    assert len(body) == total, (len(body), total)
    return bytes(body)
