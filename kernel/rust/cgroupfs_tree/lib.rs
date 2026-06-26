#![no_std]

mod ffi;

use core::ffi::{c_char, c_int, c_uint, c_void};
use core::mem::size_of;
use core::ptr;

use ffi::*;

static mut CG_GLOBAL_SB: *mut cg_sb_t = ptr::null_mut();

fn c_str_eq(ptr_s: *const c_char, lit: &[u8]) -> bool {
    if ptr_s.is_null() {
        return false;
    }
    let mut i = 0usize;
    unsafe {
        loop {
            let ch = *ptr_s.add(i) as u8;
            let want = if i < lit.len() { lit[i] } else { 0 };
            if ch != want {
                return false;
            }
            if ch == 0 {
                return i == lit.len();
            }
            i += 1;
        }
    }
}

fn c_str_eq2(a: *const c_char, b: *const c_char) -> bool {
    if a.is_null() || b.is_null() {
        return false;
    }
    let mut i = 0usize;
    unsafe {
        loop {
            let ca = *a.add(i) as u8;
            let cb = *b.add(i) as u8;
            if ca != cb {
                return false;
            }
            if ca == 0 {
                return true;
            }
            i += 1;
        }
    }
}

unsafe fn copy_c_name(dst: &mut [c_char; 64], src: *const c_char) {
    let mut i = 0usize;
    while i + 1 < dst.len() {
        let ch = unsafe { *src.add(i) };
        dst[i] = ch;
        if ch == 0 {
            return;
        }
        i += 1;
    }
    dst[dst.len() - 1] = 0;
}

unsafe fn alloc_zeroed<T>() -> *mut T {
    let p = unsafe { kmalloc(size_of::<T>()) as *mut T };
    if !p.is_null() {
        unsafe { memset(p as *mut c_void, 0, size_of::<T>()) };
    }
    p
}

unsafe fn cg_add_file(node: *mut cg_node_t, file: cg_file_t) {
    if unsafe { (*node).file_count } < CG_MAX_FILES as c_int {
        let idx = unsafe { (*node).file_count as usize };
        unsafe { (*node).files[idx] = file };
        unsafe { (*node).file_count += 1 };
    }
}

unsafe fn cg_node_populate_files(node: *mut cg_node_t, sb: *mut cg_sb_t) {
    unsafe { (*node).file_count = 0 };
    let ver = unsafe { (*sb).ver };
    let ctrls = unsafe { (*sb).controllers };
    if ver == CG_V1 {
        unsafe {
            cg_add_file(node, CF_TASKS);
            cg_add_file(node, CF_CGROUP_PROCS);
            cg_add_file(node, CF_NOTIFY_ON_RELEASE);
            cg_add_file(node, CF_RELEASE_AGENT);
            cg_add_file(node, CF_CLONE_CHILDREN);
            cg_add_file(node, CF_EVENT_CONTROL);
        }
        if (ctrls & CTRL_MEMORY) != 0 {
            unsafe {
                cg_add_file(node, CF_MEMORY_USAGE);
                cg_add_file(node, CF_MEMORY_LIMIT);
                cg_add_file(node, CF_MEMORY_MAX_USAGE);
                cg_add_file(node, CF_MEMORY_STAT);
                cg_add_file(node, CF_MEMORY_SWAPPINESS);
                cg_add_file(node, CF_MEMORY_USE_HIERARCHY);
                cg_add_file(node, CF_MEMORY_MEMSW_USAGE);
                cg_add_file(node, CF_MEMORY_MEMSW_LIMIT);
                cg_add_file(node, CF_MEMORY_KMEM_USAGE);
                cg_add_file(node, CF_MEMORY_KMEM_LIMIT);
            }
        }
        if (ctrls & CTRL_CPUSET) != 0 {
            unsafe {
                cg_add_file(node, CF_CPUSET_CPUS);
                cg_add_file(node, CF_CPUSET_MEMS);
                cg_add_file(node, CF_CPUSET_MEMORY_MIGRATE);
            }
        }
        if (ctrls & CTRL_CPU) != 0 {
            unsafe {
                cg_add_file(node, CF_CPU_CFS_QUOTA);
                cg_add_file(node, CF_CPU_CFS_PERIOD);
                cg_add_file(node, CF_CPU_SHARES);
                cg_add_file(node, CF_CPU_STAT);
            }
        }
    } else {
        unsafe {
            cg_add_file(node, CF_CGROUP_PROCS);
            cg_add_file(node, CF_CGROUP_CONTROLLERS);
            cg_add_file(node, CF_CGROUP_SUBTREE_CONTROL);
            cg_add_file(node, CF_CGROUP_KILL);
            cg_add_file(node, CF_CGROUP_TYPE);
            cg_add_file(node, CF_MEMORY_CURRENT);
            cg_add_file(node, CF_MEMORY_MAX);
            cg_add_file(node, CF_MEMORY_MIN);
            cg_add_file(node, CF_MEMORY_LOW);
            cg_add_file(node, CF_MEMORY_EVENTS);
            cg_add_file(node, CF_MEMORY_STAT);
            cg_add_file(node, CF_MEMORY_SWAP_CURRENT);
            cg_add_file(node, CF_MEMORY_SWAP_MAX);
            cg_add_file(node, CF_CPU_MAX);
            cg_add_file(node, CF_CPUSET_CPUS);
            cg_add_file(node, CF_CPUSET_MEMS);
        }
    }
}

unsafe fn cg_node_create(name: *const c_char, parent: *mut cg_node_t) -> *mut cg_node_t {
    let node = unsafe { alloc_zeroed::<cg_node_t>() };
    if node.is_null() {
        return ptr::null_mut();
    }
    unsafe {
        copy_c_name(&mut (*node).name, name);
        (*node).parent = parent;
        a20_cgroupfs_spin_init(&mut (*node).lock);
        cg_mem_init(&mut (*node).res.mem);
        cg_cpu_init(&mut (*node).res.cpu);
        cg_cpuset_init(&mut (*node).res.cpuset, 1);
    }
    node
}

unsafe fn cg_find_child_locked(parent: *mut cg_node_t, name: *const c_char) -> *mut cg_node_t {
    let count = unsafe { (*parent).child_count };
    let mut i = 0;
    while i < count {
        let child = unsafe { (*parent).children[i as usize] };
        if !child.is_null() && c_str_eq2(unsafe { (*child).name.as_ptr() }, name) {
            return child;
        }
        i += 1;
    }
    ptr::null_mut()
}

unsafe fn cg_find_file_by_name(node: *mut cg_node_t, sb: *mut cg_sb_t, name: *const c_char) -> cg_file_t {
    let count = unsafe { (*node).file_count };
    let mut i = 0;
    while i < count {
        let file = unsafe { (*node).files[i as usize] };
        let fname = unsafe { a20_cgroupfs_file_name(file, (*sb).ver) };
        if !fname.is_null() && c_str_eq2(name, fname) {
            return file;
        }
        i += 1;
    }
    if unsafe { (*sb).ver } == CG_V1 && (unsafe { (*sb).controllers } & CTRL_CPUSET) != 0 {
        if c_str_eq(name, b"cpus") {
            return CF_CPUSET_CPUS;
        }
        if c_str_eq(name, b"mems") {
            return CF_CPUSET_MEMS;
        }
        if c_str_eq(name, b"memory_migrate") {
            return CF_CPUSET_MEMORY_MIGRATE;
        }
    }
    CF_FILE_MAX
}

unsafe fn cg_node_free_recursive(node: *mut cg_node_t) {
    if node.is_null() {
        return;
    }
    let count = unsafe { (*node).child_count };
    let mut i = 0;
    while i < count {
        let child = unsafe { (*node).children[i as usize] };
        unsafe { cg_node_free_recursive(child) };
        i += 1;
    }
    unsafe { kfree(node as *mut c_void) };
}

unsafe fn make_dir_vnode(dir: *mut vnode_t, child: *mut cg_node_t) -> *mut vnode_t {
    let vn = unsafe { alloc_zeroed::<vnode_t>() };
    if vn.is_null() {
        return ptr::null_mut();
    }
    unsafe {
        (*vn).ino = child as usize as u64;
        (*vn).type_ = VFS_FT_DIR;
        (*vn).mode = S_IFDIR | 0o755;
        (*vn).parent = dir;
        vnode_get(dir);
        (*vn).ops = &raw mut G_CG_VNODE_OPS;
        (*vn).mnt = (*dir).mnt;
        vnode_ref_init(vn, 1);
        (*vn).fs_data = child as *mut c_void;
        (*vn).uid = (*child).uid;
        (*vn).gid = (*child).gid;
    }
    vn
}

unsafe fn make_file_vnode(dir: *mut vnode_t, node: *mut cg_node_t, sb: *mut cg_sb_t, file: cg_file_t) -> *mut vnode_t {
    let vn = unsafe { alloc_zeroed::<vnode_t>() };
    if vn.is_null() {
        return ptr::null_mut();
    }
    unsafe {
        (*vn).type_ = VFS_FT_REGULAR;
        (*vn).mode = S_IFREG | if a20_cgroupfs_file_writable(file) != 0 { 0o644 } else { 0o444 };
        (*vn).parent = dir;
        vnode_get(dir);
        (*vn).ops = &raw mut G_CG_VNODE_OPS;
        (*vn).mnt = (*dir).mnt;
        vnode_ref_init(vn, 1);
        (*vn).size = a20_cgroupfs_file_size(file, sb, node).max(0) as usize;
        (*vn).fs_data = node as *mut c_void;
        (*vn).uid = (*node).uid;
        (*vn).gid = (*node).gid;
        (*vn).ino = (file as u64 + 1) | ((node as usize as u64) << 16);
    }
    vn
}

unsafe fn cg_get_sb(vn: *mut vnode_t) -> *mut cg_sb_t {
    if vn.is_null() || unsafe { (*vn).mnt.is_null() } {
        return ptr::null_mut();
    }
    unsafe { (*(*vn).mnt).fs_data as *mut cg_sb_t }
}

fn parse_controllers(opts: *const c_char) -> u32 {
    if opts.is_null() {
        return 0;
    }
    let mut ctrl = 0u32;
    let mut token = [0u8; 32];
    let mut ti = 0usize;
    let mut p = opts;
    unsafe {
        loop {
            let ch = *p as u8;
            if ch == 0 || ch == b',' {
                if ti != 0 {
                    if token[..ti] == *b"memory" {
                        ctrl |= CTRL_MEMORY;
                    } else if token[..ti] == *b"cpu" {
                        ctrl |= CTRL_CPU;
                    } else if token[..ti] == *b"cpuset" {
                        ctrl |= CTRL_CPUSET;
                    } else if token[..ti] == *b"cpuacct" {
                        ctrl |= CTRL_CPUACCT;
                    }
                    ti = 0;
                }
                if ch == 0 {
                    break;
                }
            } else if ti + 1 < token.len() && ch != b' ' && ch != b'\t' && ch != b'\n' {
                token[ti] = ch;
                ti += 1;
            }
            p = p.add(1);
        }
    }
    ctrl
}

extern "C" fn cg_lookup(dir: *mut vnode_t, name: *const c_char, out: *mut *mut vnode_t) -> c_int {
    unsafe {
        let sb = cg_get_sb(dir);
        if sb.is_null() {
            return -ENOENT;
        }
        let node = (*dir).fs_data as *mut cg_node_t;
        if node.is_null() {
            return -ENOENT;
        }
        if c_str_eq(name, b".") {
            *out = dir;
            vnode_get(dir);
            return 0;
        }
        if c_str_eq(name, b"..") {
            if !(*node).parent.is_null() && !(*dir).parent.is_null() {
                *out = (*dir).parent;
                vnode_get(*out);
                return 0;
            }
            *out = dir;
            vnode_get(dir);
            return 0;
        }

        let flags = a20_cgroupfs_spin_lock_irqsave(&mut (*node).lock);
        let child = cg_find_child_locked(node, name);
        let file = if child.is_null() { cg_find_file_by_name(node, sb, name) } else { CF_FILE_MAX };
        a20_cgroupfs_spin_unlock_irqrestore(&mut (*node).lock, flags);

        if !child.is_null() {
            let vn = make_dir_vnode(dir, child);
            if vn.is_null() {
                return -ENOMEM;
            }
            *out = vn;
            return 0;
        }
        if file != CF_FILE_MAX {
            let vn = make_file_vnode(dir, node, sb, file);
            if vn.is_null() {
                return -ENOMEM;
            }
            *out = vn;
            return 0;
        }
        -ENOENT
    }
}

extern "C" fn cg_create(_dir: *mut vnode_t, _name: *const c_char, _mode: c_int, _out: *mut *mut vnode_t) -> c_int {
    -EROFS
}

extern "C" fn cg_mkdir(dir: *mut vnode_t, name: *const c_char, _mode: c_int) -> c_int {
    unsafe {
        let parent = (*dir).fs_data as *mut cg_node_t;
        let sb = cg_get_sb(dir);
        if parent.is_null() || sb.is_null() {
            return -ENOENT;
        }
        let child = cg_node_create(name, parent);
        if child.is_null() {
            return -ENOMEM;
        }
        cg_node_populate_files(child, sb);
        let flags = a20_cgroupfs_spin_lock_irqsave(&mut (*parent).lock);
        if (*parent).child_count >= CG_MAX_CHILDREN as c_int {
            a20_cgroupfs_spin_unlock_irqrestore(&mut (*parent).lock, flags);
            kfree(child as *mut c_void);
            return -ENOSPC;
        }
        if !cg_find_child_locked(parent, name).is_null() || cg_find_file_by_name(parent, sb, name) != CF_FILE_MAX {
            a20_cgroupfs_spin_unlock_irqrestore(&mut (*parent).lock, flags);
            kfree(child as *mut c_void);
            return -EEXIST;
        }
        let idx = (*parent).child_count as usize;
        (*parent).children[idx] = child;
        (*parent).child_count += 1;
        a20_cgroupfs_spin_unlock_irqrestore(&mut (*parent).lock, flags);
        0
    }
}

extern "C" fn cg_unlink(_dir: *mut vnode_t, _name: *const c_char) -> c_int {
    -EROFS
}

extern "C" fn cg_rmdir(dir: *mut vnode_t, name: *const c_char) -> c_int {
    unsafe {
        let parent = (*dir).fs_data as *mut cg_node_t;
        if parent.is_null() {
            return -ENOENT;
        }
        let flags = a20_cgroupfs_spin_lock_irqsave(&mut (*parent).lock);
        let count = (*parent).child_count;
        let mut idx = -1;
        let mut victim = ptr::null_mut();
        let mut i = 0;
        while i < count {
            let child = (*parent).children[i as usize];
            if !child.is_null() && c_str_eq2((*child).name.as_ptr(), name) {
                idx = i;
                victim = child;
                break;
            }
            i += 1;
        }
        if idx < 0 {
            a20_cgroupfs_spin_unlock_irqrestore(&mut (*parent).lock, flags);
            return -ENOENT;
        }
        let last = ((*parent).child_count - 1) as usize;
        (*parent).children[idx as usize] = (*parent).children[last];
        (*parent).children[last] = ptr::null_mut();
        (*parent).child_count -= 1;
        a20_cgroupfs_spin_unlock_irqrestore(&mut (*parent).lock, flags);
        cg_node_free_recursive(victim);
        0
    }
}

extern "C" fn cg_rename(_od: *mut vnode_t, _on: *const c_char, _nd: *mut vnode_t, _nn: *const c_char, _flags: c_uint) -> c_int {
    -EROFS
}

extern "C" fn cg_stat(vn: *mut vnode_t, st: *mut kstat_t) -> c_int {
    unsafe {
        memset(st as *mut c_void, 0, size_of::<kstat_t>());
        let node = (*vn).fs_data as *mut cg_node_t;
        (*st).st_ino = (*vn).ino;
        (*st).st_mode = (*vn).mode;
        (*st).st_uid = if node.is_null() { 0 } else { (*node).uid };
        (*st).st_gid = if node.is_null() { 0 } else { (*node).gid };
        (*st).st_size = (*vn).size as u64;
        (*st).st_nlink = 1;
        0
    }
}

extern "C" fn cg_chown(vn: *mut vnode_t, uid: c_int, gid: c_int) -> c_int {
    unsafe {
        let node = (*vn).fs_data as *mut cg_node_t;
        if uid != -1 {
            (*vn).uid = uid as u32;
            if !node.is_null() {
                (*node).uid = uid as u32;
            }
        }
        if gid != -1 {
            (*vn).gid = gid as u32;
            if !node.is_null() {
                (*node).gid = gid as u32;
            }
        }
        0
    }
}

extern "C" fn cgroupfs_open_vnode(vn: *mut vnode_t, flags: c_int) -> *mut vfile_t {
    unsafe { a20_cgroupfs_open_vnode(vn, flags) }
}

extern "C" fn cg_release(vn: *mut vnode_t) {
    unsafe {
        if !(*vn).parent.is_null() {
            vnode_put((*vn).parent);
        }
        kfree(vn as *mut c_void);
    }
}

#[unsafe(no_mangle)]
pub static mut G_CG_VNODE_OPS: vnode_ops_t = vnode_ops_t {
    lookup: Some(cg_lookup),
    create: Some(cg_create),
    mkdir: Some(cg_mkdir),
    unlink: Some(cg_unlink),
    rmdir: Some(cg_rmdir),
    rename: Some(cg_rename),
    link: None,
    symlink: None,
    readlink: None,
    stat: Some(cg_stat),
    truncate: None,
    writepage: None,
    chmod: None,
    chown: Some(cg_chown),
    open: Some(cgroupfs_open_vnode),
    release: Some(cg_release),
};

#[unsafe(no_mangle)]
pub extern "C" fn cgroupfs_mount(is_v2: c_int, opts: *const c_char, out_sb: *mut *mut c_void) -> *mut vnode_t {
    unsafe {
        let sb = alloc_zeroed::<cg_sb_t>();
        if sb.is_null() {
            return ptr::null_mut();
        }
        (*sb).ver = if is_v2 != 0 { CG_V2 } else { CG_V1 };
        if is_v2 != 0 {
            (*sb).controllers = CTRL_MEMORY | CTRL_CPU | CTRL_CPUSET | CTRL_CPUACCT;
        } else {
            let parsed = parse_controllers(opts);
            (*sb).controllers = if parsed == 0 {
                CTRL_MEMORY | CTRL_CPU | CTRL_CPUSET | CTRL_CPUACCT
            } else {
                parsed
            };
        }

        let root_name = [0 as c_char; 1];
        let root = cg_node_create(root_name.as_ptr(), ptr::null_mut());
        if root.is_null() {
            kfree(sb as *mut c_void);
            return ptr::null_mut();
        }
        (*root).is_root = 1;
        cg_node_populate_files(root, sb);
        (*sb).root = root;

        let vn = alloc_zeroed::<vnode_t>();
        if vn.is_null() {
            cg_node_free_recursive(root);
            kfree(sb as *mut c_void);
            return ptr::null_mut();
        }
        (*vn).ino = 0;
        (*vn).type_ = VFS_FT_DIR;
        (*vn).mode = S_IFDIR | 0o755;
        vnode_ref_init(vn, 1);
        (*vn).ops = &raw mut G_CG_VNODE_OPS;
        (*vn).fs_data = root as *mut c_void;
        if !out_sb.is_null() {
            *out_sb = sb as *mut c_void;
        }
        CG_GLOBAL_SB = sb;
        vn
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn cgroupfs_unmount(root: *mut vnode_t) {
    unsafe {
        if root.is_null() {
            return;
        }
        let node = (*root).fs_data as *mut cg_node_t;
        if !node.is_null() {
            cg_node_free_recursive(node);
        }
        if !(*root).mnt.is_null() && !(*(*root).mnt).fs_data.is_null() {
            if CG_GLOBAL_SB == (*(*root).mnt).fs_data as *mut cg_sb_t {
                CG_GLOBAL_SB = ptr::null_mut();
            }
            kfree((*(*root).mnt).fs_data);
            (*(*root).mnt).fs_data = ptr::null_mut();
        }
        kfree(root as *mut c_void);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn cg_root_node() -> *mut cg_node_t {
    unsafe {
        if CG_GLOBAL_SB.is_null() {
            ptr::null_mut()
        } else {
            (*CG_GLOBAL_SB).root
        }
    }
}
