#include <libmtp.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

/* Read-only MTP operations. Only the local output directory is written. */
static FILE *manifest;
static unsigned files, folders, errors;
static uint64_t total, sample_size = UINT64_MAX;
static uint32_t sample_id;
static char *sample_name;
static int first = 1;

static void quoted(const char *s) {
  fputc('"', manifest);
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    if (*p == '"' || *p == '\\') fprintf(manifest, "\\%c", *p);
    else if (*p < 32) fprintf(manifest, "\\u%04x", *p);
    else fputc(*p, manifest);
  }
  fputc('"', manifest);
}

static void walk(LIBMTP_mtpdevice_t *d, uint32_t storage, uint32_t parent,
                 const char *prefix, unsigned depth) {
  if (depth > 64) { errors++; return; }
  LIBMTP_file_t *head = LIBMTP_Get_Files_And_Folders(d, storage, parent);
  if (LIBMTP_Get_Errorstack(d)) {
    LIBMTP_Dump_Errorstack(d); LIBMTP_Clear_Errorstack(d); errors++;
  }
  while (head) {
    LIBMTP_file_t *f = head;
    head = f->next;
    char *path = NULL;
    if (asprintf(&path, "%s/%s", prefix, f->filename ? f->filename : "") < 0) exit(2);
    int folder = f->filetype == LIBMTP_FILETYPE_FOLDER;
    if (!first) fputs(",\n", manifest);
    first = 0;
    fprintf(manifest, "  {\"id\":%u,\"parent_id\":%u,\"storage_id\":%u,\"folder\":%s,\"size\":%llu,\"modified\":%lld,\"path\":",
            f->item_id, f->parent_id, f->storage_id, folder ? "true" : "false",
            (unsigned long long)f->filesize, (long long)f->modificationdate);
    quoted(path); fputc('}', manifest); fflush(manifest);
    if (folder) {
      folders++;
      printf("Folder %s\n", path); fflush(stdout);
      walk(d, storage, f->item_id, path, depth + 1);
    } else {
      files++; total += f->filesize;
      const char *ext = strrchr(f->filename ? f->filename : "", '.');
      if (ext && (!strcasecmp(ext,".wav") || !strcasecmp(ext,".mp3") ||
                  !strcasecmp(ext,".m4a") || !strcasecmp(ext,".opus")) &&
          f->filesize > 0 && f->filesize < sample_size) {
        sample_id = f->item_id; sample_size = f->filesize;
        free(sample_name); sample_name = strdup(path);
      }
    }
    free(path); LIBMTP_destroy_file_t(f);
  }
}

int main(int argc, char **argv) {
  if (argc != 2) { fprintf(stderr,"Usage: %s OUTPUT_DIRECTORY\n",argv[0]); return 2; }
  if (mkdir(argv[1],0700)) { perror("Create new output directory"); return 2; }
  LIBMTP_Init();
  LIBMTP_raw_device_t *raw = NULL; int count = 0;
  if (LIBMTP_Detect_Raw_Devices(&raw,&count)) return 1;
  LIBMTP_mtpdevice_t *d = NULL;
  for (int i=0;i<count;i++) {
    if (raw[i].device_entry.vendor_id == 0x2207 && raw[i].device_entry.product_id == 1) {
      d = LIBMTP_Open_Raw_Device_Uncached(&raw[i]); break;
    }
  }
  free(raw);
  if (!d) { fprintf(stderr,"Cannot open recorder\n"); return 1; }
  char *model = LIBMTP_Get_Modelname(d);
  printf("Connected: %s\n",model ? model : "unknown"); fflush(stdout);
  if (!model || strcmp(model,"E1 AI Recorder")) {
    free(model); LIBMTP_Release_Device(d); return 1;
  }
  free(model);
  char *dest = NULL; asprintf(&dest,"%s/inventory.json",argv[1]);
  manifest = fopen(dest,"wx"); free(dest);
  if (!manifest) { perror("inventory"); LIBMTP_Release_Device(d); return 1; }
  fputs("[\n",manifest);
  for (LIBMTP_devicestorage_t *s=d->storage;s;s=s->next) {
    printf("Storage %u: %s, capacity %llu, free %llu bytes\n",s->id,s->StorageDescription,
           (unsigned long long)s->MaxCapacity,(unsigned long long)s->FreeSpaceInBytes);
    walk(d,s->id,LIBMTP_FILES_AND_FOLDERS_ROOT,"",0);
  }
  fputs("\n]\n",manifest); fclose(manifest);
  printf("Listed %u files, %u folders, %llu bytes; %u errors\n",files,folders,(unsigned long long)total,errors);
  int result = errors ? 1 : 0;
  if (sample_id && sample_size <= 50*1024*1024) {
    const char *ext = strrchr(sample_name,'.');
    asprintf(&dest,"%s/sample%s",argv[1],ext);
    int fd = open(dest,O_WRONLY|O_CREAT|O_EXCL,0600);
    if (fd < 0) { perror("sample"); result=1; }
    else {
      printf("Reading %s (%llu bytes) to %s\n",sample_name,(unsigned long long)sample_size,dest); fflush(stdout);
      int rc = LIBMTP_Get_File_To_File_Descriptor(d,sample_id,fd,NULL,NULL);
      struct stat st; int valid = !fstat(fd,&st) && (uint64_t)st.st_size == sample_size;
      close(fd);
      printf("Sample download: %s\n",rc == 0 && valid ? "OK (size verified)" : "FAILED");
      if (rc || !valid) { LIBMTP_Dump_Errorstack(d); result=1; }
    }
    free(dest);
  }
  free(sample_name); LIBMTP_Release_Device(d);
  return result;
}
