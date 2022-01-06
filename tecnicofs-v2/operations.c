#include "operations.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int tfs_init() {
    state_init();

    /* create root inode */
    int root = inode_create(T_DIRECTORY);
    if (root != ROOT_DIR_INUM) {
        return -1;
    }

    return 0;
}

int tfs_destroy() {
    state_destroy();
    return 0;
}

static bool valid_pathname(char const *name) {
    return name != NULL && strlen(name) > 1 && name[0] == '/';
}


int tfs_lookup(char const *name) {
    if (!valid_pathname(name)) {
        return -1;
    }

    // skip the initial '/' character
    name++;

    return find_in_dir(ROOT_DIR_INUM, name);
}

int tfs_open(char const *name, int flags) {
    int inum;
    size_t offset;

    /* Checks if the path name is valid */
    if (!valid_pathname(name)) {
        return -1;
    }

    inum = tfs_lookup(name);
    if (inum >= 0) {
        /* The file already exists */
        inode_t *inode = inode_get(inum);
        if (inode == NULL) {
            return -1;
        }
        /* Trucate (if requested) */
        if (flags & TFS_O_TRUNC) {
            if (inode->i_size > 0) {
                int free = 0;
                for(int i = 0; i<10; i++){
                    if (data_block_free(inode->i_data_block[i]) != -1)
                        free = 1;
                }
                if (free == 0)
                    return -1;

                // vai buscar o numero do bloco indireto, depois vamos encontrar o ponteiro para esse bloco,
                // dentro desse bloco vao estar os numeros dos blocos que o inode ocupa e vamos ter de dar free nesses blocos
                if(inode->indirect_block != -1){
                    int * p_ind_block = (int *) data_block_get(inode->indirect_block);
                    for(int *p_aux = p_ind_block; *p_aux >= 0 && *p_aux < 1024; p_aux += sizeof(int))
                        data_block_free(*p_aux);
                }
            inode->i_size = 0;
            }
        }
        /* Determine initial offset */
        if (flags & TFS_O_APPEND) {
            offset = inode->i_size;
        } else {
            offset = 0;
        }
    } else if (flags & TFS_O_CREAT) {
        /* The file doesn't exist; the flags specify that it should be created*/
        /* Create inode */
        inum = inode_create(T_FILE);
        if (inum == -1) {
            return -1;
        }
        /* Add entry in the root directory */
        if (add_dir_entry(ROOT_DIR_INUM, inum, name + 1) == -1) {
            inode_delete(inum);
            return -1;
        }
        offset = 0;
    } else {
        return -1;
    }

    /* Finally, add entry to the open file table and
     * return the corresponding handle */
    return add_to_open_file_table(inum, offset);

    /* Note: for simplification, if file was created with TFS_O_CREAT and there
     * is an error adding an entry to the open file table, the file is not
     * opened but it remains created */
}


int tfs_close(int fhandle) { return remove_from_open_file_table(fhandle); }

ssize_t tfs_write(int fhandle, void const *buffer, size_t to_write) {
    open_file_entry_t *file = get_open_file_entry(fhandle);
    if (file == NULL) {
        return -1;
    }

    /* From the open file table entry, we get the inode */
    inode_t *inode = inode_get(file->of_inumber);
    if (inode == NULL) {
        return -1;
    }

    /* Determine how many bytes to write */
    if (to_write + file->of_offset > 10*BLOCK_SIZE + (BLOCK_SIZE/sizeof(int))*BLOCK_SIZE) {
        to_write = (10*BLOCK_SIZE + (BLOCK_SIZE/sizeof(int))*BLOCK_SIZE) - file->of_offset;
    }
    size_t current_write;
    if (to_write > 0) {
        for(size_t resto = to_write; resto > 0; resto -= current_write){
            //estamos no inicio ou acabamos um bloco
            if(inode->i_size % BLOCK_SIZE == 0){
                //alocar o proximo bloco
                int block = alloc_next_block(inode);
                void *p_block = data_block_get(block);
                //vamos preencher o bloco ate ao fim
                if(resto - BLOCK_SIZE > 0){
                    current_write = BLOCK_SIZE;
                    memcpy(p_block, buffer + to_write - resto, current_write);
                }
                //so vamos preencher uma parte do bloco e acaba
                else{
                    current_write = resto;
                    memcpy(p_block, buffer + to_write - resto, current_write);
                }
            }
            //se estivermos a meio de um bloco
            else{
                //procurar o bloco em que está o ficheiro
                //buscar o ponteiro
                //calcular onde estamos no bloco
                size_t block_offset = inode->i_size % BLOCK_SIZE;
                //ver o que ainda podemos escrever no bloco
                current_write = BLOCK_SIZE - block_offset;
                //preencher o resto do bloco
                if(resto - current_write > 0){
                    memcpy(p_block + block_offset, buffer + to_write - resto, current_write);
                }
                //so vamos preencher uma parte do bloco
                else{
                    current_write = resto;
                    memcpy(p_block + block_offset, buffer + to_write - resto, current_write);
                }
                
            }
            void *block = data_block_get(inode->i_data_block);
            if (block == NULL) {
                return -1;
            }
        }
        /* The offset associated with the file handle is
         * incremented accordingly */
        file->of_offset += to_write;
        if (file->of_offset > inode->i_size) {
            inode->i_size = file->of_offset;
        }
    }
    return (ssize_t)to_write;
}


ssize_t tfs_read(int fhandle, void *buffer, size_t len) {
    open_file_entry_t *file = get_open_file_entry(fhandle);
    if (file == NULL) {
        return -1;
    }

    /* From the open file table entry, we get the inode */
    inode_t *inode = inode_get(file->of_inumber);
    if (inode == NULL) {
        return -1;
    }

    /* Determine how many bytes to read */
    size_t to_read = inode->i_size - file->of_offset;
    if (to_read > len) {
        to_read = len;
    }

    if (to_read > 0) {
        void *block = data_block_get(inode->i_data_block);
        if (block == NULL) {
            return -1;
        }

        /* Perform the actual read */
        memcpy(buffer, block + file->of_offset, to_read);
        /* The offset associated with the file handle is
         * incremented accordingly */
        file->of_offset += to_read;
    }

    return (ssize_t)to_read;
}

int tfs_copy_to_external_fs(char const *source_path, char const *dest_path){
    int fdest,fsource,cnt;
    char buffer[256];

    fsource = tfs_open(source_path, TFS_O_APPEND);

    if (fsource == 0){
        perror(source_path);
        return -1;
    }
    //cria o ficheiro ou substitui o conteudo
    fdest = tfs_open(dest_path,TFS_O_CREAT|TFS_O_TRUNC);

    if (fdest == 0){
        perror(dest_path);
        tfs_close(fsource);
        return -1;
    }

    while((cnt = tfs_read(fsource,buffer,256)) > 0){
        if (tfs_write(fdest,buffer,cnt) < cnt){
            perror(dest_path);
            tfs_close(fsource);
            tfs_close(fdest);
            return -1;
        }
    }

    if(cnt<0){
        perror(source_path);
        tfs_close(fsource);
        tfs_close(fdest);
        return -1;
    }

    tfs_close(fsource);
    tfs_close(fdest);

    return 0; 
}