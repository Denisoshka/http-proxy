#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "proxy.h"
#include "../utils/log.h"

typedef struct UploadArgs {
  CacheEntryT *entry;
  BufferT *    buffer;
  int          remoteSocket;
  int          clientSocket;
} UploadArgsT;

static CacheEntryChunkT *fillCache(CacheEntryChunkT *startChunk,
                                   CacheEntryT *     entry,
                                   const BufferT *   buffer) {
  size_t            added = 0;
  CacheEntryChunkT *curChunk = startChunk;
  while (added != buffer->occupancy) {
    const size_t dif = curChunk->maxDataSize - curChunk->curDataSize;
    if (dif == 0) {
      CacheEntryT_append_CacheEntryChunkT(entry, curChunk, 0);
      curChunk = CacheEntryChunkT_new(kDefCacheChunkSize);
      if (curChunk == NULL) {
        return NULL;
      }
    } else {
      memcpy(
        curChunk->data + curChunk->curDataSize, buffer->data + added, dif
      );
      curChunk->curDataSize += dif;
      added += dif;
    }
  }
  return curChunk;
}

static CacheEntryChunkT *readRestBytes(
  const int    clientSocket,
  const int    remoteSocket,
  CacheEntryT *entry,
  BufferT *    buffer
) {
  CacheEntryChunkT *curChunk = CacheEntryChunkT_new(kDefCacheChunkSize);
  if (curChunk == NULL) {
    return NULL;
  }
  while (1) {
    const ssize_t readed = recvN(
      clientSocket, buffer->data, buffer->maxSize
    );
    if (readed < 0) {
      return NULL;
    }
    if (readed == 0) {
      break;
    }
    buffer->occupancy = readed;

    curChunk = fillCache(curChunk, entry, buffer);
    if (curChunk == NULL) return NULL;
    CacheEntryT_updateStatus(entry, InProcess);
    const ssize_t written = sendN(
      remoteSocket, buffer->data, buffer->occupancy
    );
    if (written < 0) {
      return NULL;
    }
  }

  return curChunk;
}

char *strstrn(const char * haystack,
              const size_t haystackLen,
              const char * needle,
              const size_t needleLen) {
  // Если подстрока пуста, сразу возвращаем указатель на начало строки
  if (needleLen == 0) {
    return (char *) haystack;
  }

  // Проходим по строке до максимальной длины
  for (size_t i = 0; i <= haystackLen - needleLen; i++) {
    // Сравниваем текущий участок с подстрокой
    if (strncmp(&haystack[i], needle, needleLen) == 0) {
      return (char *) &haystack[i]; // Подстрока найдена
    }
  }

  return NULL; // Подстрока не найдена
}

int isStatusCode200(const char *response, ) {
  return strstrn(response, "HTTP/1.1 200 OK", ) != NULL || strstr(
           response, "HTTP/1.0 200 OK") != NULL;
}

void *fileUploaderStartup(void *args) {
  UploadArgsT *uploadArgs = args;
  CacheEntryT *entry = uploadArgs->entry;
  BufferT *    buffer = uploadArgs->buffer;
  const int    remoteSocket = uploadArgs->remoteSocket;
  const int    clientSocket = uploadArgs->clientSocket;

  CacheEntryChunkT *curChunk = readRestBytes(
    clientSocket, remoteSocket, entry, buffer
  );
  if (curChunk == NULL) {
    logError("%s:%d readRestBytes %s", __FILE__,__LINE__, strerror(errno));
    CacheEntryT_updateStatus(entry, Failed);
    goto dectroyContext;
  }

  int uploadStatus = SUCCESS;
  while (1) {
    const ssize_t readed = recvN(
      remoteSocket, buffer->data, buffer->maxSize
    );
    if (readed < 0) {
      logError("%s:%d recv %s",__FILE__, __LINE__, strerror(errno));
      uploadStatus = ERROR;
      break;
    }
    if (readed == 0) {
      break;
    }
    buffer->occupancy = readed;
    curChunk = fillCache(curChunk, entry, buffer);
    if (curChunk == NULL) {
      logError("%s:%d fillCache %s",__FILE__, __LINE__, strerror(errno));
      uploadStatus = ERROR;
      break;
    }
    CacheEntryT_updateStatus(entry, InProcess);
  }

  if (uploadStatus != SUCCESS) {
    CacheEntryT_updateStatus(entry, Failed);
  } else {
    CacheEntryT_updateStatus(entry, Success);
  }

dectroyContext:
  free(uploadArgs);
  return NULL;
}


int handleFileUpload(CacheEntryT *entry,
                     const char * host,
                     const char * path,
                     const int    port,
                     const int    clientSocket,
                     BufferT *    buffer) {
  const int remoteSocket = getSocketOfRemote(host, port);
  if (remoteSocket < 0) {
    logError("%s, %d failed getSocketOfRemote of host:port %s:%d",
             __FILE__, __LINE__, host, port);
    sendError(clientSocket, BadGatewayStatus, FailedToConnectRemoteServer);
    goto destroyContext;
  }
  pthread_t    thread;
  UploadArgsT *args = malloc(sizeof(*args));
  if (args == NULL) {
    logError("%s, %d malloc", __FILE__, __LINE__);
    goto uploadFailed;
  }
  args->buffer = buffer;
  args->remoteSocket = remoteSocket;
  args->clientSocket = clientSocket;
  args->entry = entry;
  const int ret = pthread_create(&thread,NULL, fileUploaderStartup, args);
  if (ret != 0) {
    logError("%s, %d pthread_create", __FILE__, __LINE__);
    goto uploadFailed;
  }
  if (pthread_detach(thread) != 0) {
    logError("%s, %d pthread_detach", __FILE__, __LINE__);
    abort();
  }
  return SUCCESS;

uploadFailed:
  sendError(clientSocket, InternalErrorStatus, "");
destroyContext:
  free(args);
  return ERROR;
}
