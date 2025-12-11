BricksVFS

A lightweight, single-file virtual file system (VFS) with an HTTP interface.

BricksVFS is a C++ project that implements a custom file system in a single .db container file (similar to a virtual disk image). It includes a built-in HTTP server to manage files via a web browser.

Key Features:

Virtual Disk: Stores thousands of files inside one or more .db container files.

Web Interface: Drag-and-drop upload, file preview, search, and folder management.

No Database Server Needed: Pure C++ implementation using standard file I/O.

Crash Recovery: Simple journaling mechanism to prevent corruption during power loss.

Global Search: Find files instantly across the entire virtual disk.

Technical Documentation

Architecture

The file system consists of:

SuperBlock: Header containing disk geometry and signature (BRVFS).

Journal: A reserved area for atomic write operations.

Bitmap: Tracks used/free blocks.

Root Directory: Entry point for the file tree.

FAT (File Allocation Table): Linked list structure to track data blocks.

Data Blocks: Actual file content (4KB per block).

Building

Requires a C++20 compiler.

Download and place the header files in the project folder:

stb_image.h, stb_image_write.h, stb_image_resize2.h from nothings/stb

httplib.h from yhirose/cpp-httplib

Compile: g++ server.cpp VFS_Core.cpp -o server.exe -std=c++20 -lws2_32 (Windows) or -lpthread (Linux).

Configuration

Edit vfs.conf to change settings like port, root password, or disk size.
