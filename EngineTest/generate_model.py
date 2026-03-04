#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import struct
import array
import os

def create_cube_model(output_path="model.model"):
    # Cube Data
    cube_vertices = [
        # Front
        -0.5, -0.5, 0.5,  0.5, -0.5, 0.5,  0.5, 0.5, 0.5,  -0.5, 0.5, 0.5,
        # Back
        -0.5, -0.5, -0.5, 0.5, -0.5, -0.5, 0.5, 0.5, -0.5, -0.5, 0.5, -0.5
    ]
    
    cube_normals = [
        # Front (0,0,1)
        0.0, 0.0, 1.0, 0.0, 0.0, 1.0, 0.0, 0.0, 1.0, 0.0, 0.0, 1.0,
        # Back (0,0,-1)
        0.0, 0.0, -1.0, 0.0, 0.0, -1.0, 0.0, 0.0, -1.0, 0.0, 0.0, -1.0
    ]
    
    # We need 24 vertices for a cube with proper normals (4 per face * 6 faces).
    # The previous generator used 8 vertices which is wrong for flat shading/normals.
    # But for simplicity, let's stick to the minimal data or just correct it.
    # Let's generate a proper cube with 24 vertices to be safe.
    
    vertices = []
    normals = []
    indices = []
    
    # Helper to add face
    def add_face(p1, p2, p3, p4, n):
        idx = len(vertices) // 3
        vertices.extend(p1); normals.extend(n)
        vertices.extend(p2); normals.extend(n)
        vertices.extend(p3); normals.extend(n)
        vertices.extend(p4); normals.extend(n)
        # Tri 1: 0, 1, 2
        indices.extend([idx, idx+1, idx+2])
        # Tri 2: 0, 2, 3
        indices.extend([idx, idx+2, idx+3])

    # Front (z=0.5)
    add_face([-0.5, -0.5, 0.5], [0.5, -0.5, 0.5], [0.5, 0.5, 0.5], [-0.5, 0.5, 0.5], [0, 0, 1])
    # Back (z=-0.5)
    add_face([0.5, -0.5, -0.5], [-0.5, -0.5, -0.5], [-0.5, 0.5, -0.5], [0.5, 0.5, -0.5], [0, 0, -1])
    # Right (x=0.5)
    add_face([0.5, -0.5, 0.5], [0.5, -0.5, -0.5], [0.5, 0.5, -0.5], [0.5, 0.5, 0.5], [1, 0, 0])
    # Left (x=-0.5)
    add_face([-0.5, -0.5, -0.5], [-0.5, -0.5, 0.5], [-0.5, 0.5, 0.5], [-0.5, 0.5, -0.5], [-1, 0, 0])
    # Top (y=0.5)
    add_face([-0.5, 0.5, 0.5], [0.5, 0.5, 0.5], [0.5, 0.5, -0.5], [-0.5, 0.5, -0.5], [0, 1, 0])
    # Bottom (y=-0.5)
    add_face([-0.5, -0.5, -0.5], [0.5, -0.5, -0.5], [0.5, -0.5, 0.5], [-0.5, -0.5, 0.5], [0, -1, 0])

    pos_bytes = array.array('f', vertices).tobytes()
    norm_bytes = array.array('f', normals).tobytes()
    # Use uint16 for indices
    idx_bytes = array.array('H', indices).tobytes()
    
    num_vertices = len(vertices) // 3
    num_indices = len(indices)
    
    with open(output_path, 'wb') as f:
        # 1. Scene Name
        scene_name = b"CubeScene"
        f.write(struct.pack('I', len(scene_name)))
        f.write(scene_name)
        
        # 2. Num Materials
        f.write(struct.pack('I', 0))
        
        # 3. Num LOD Groups
        f.write(struct.pack('I', 1))
        
        # --- LOD Group 0 ---
        # LOD Name
        lod_name = b"CubeLOD"
        f.write(struct.pack('I', len(lod_name)))
        f.write(lod_name)
        
        # Num Meshes
        f.write(struct.pack('I', 1))
        
        # --- Mesh 0 ---
        # Mesh Name
        mesh_name = b"Cube"
        f.write(struct.pack('I', len(mesh_name)))
        f.write(mesh_name)
        
        # LOD ID
        f.write(struct.pack('I', 0))
        
        # Vertex Size (Pos(12) + Norm(12) = 24)
        f.write(struct.pack('I', 24))
        
        # Num Vertices
        f.write(struct.pack('I', num_vertices))
        
        # Index Size (2 for uint16)
        f.write(struct.pack('I', 2))
        
        # Num Indices
        f.write(struct.pack('I', num_indices))
        
        # Debug info
        print(f"VertexSize: 24, NumVertices: {num_vertices}, IndexSize: 2, NumIndices: {num_indices}")
        
        # LOD Threshold
        f.write(struct.pack('f', 0.0))
        
        # Position Data
        f.write(pos_bytes)
        
        # Element Data (Normals)
        f.write(norm_bytes)
        
        # Index Data
        f.write(idx_bytes)
        
        # Material Index (-1)
        f.write(struct.pack('i', -1))

    print(f"Generated {output_path}")

if __name__ == "__main__":
    create_cube_model()
