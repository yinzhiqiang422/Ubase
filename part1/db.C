/*****************************************************
* 产品名称: UBASE                                    *
* 大学名称: 新疆农业大学                             *
* 学院名称: 计算机与信息工程学院                     *
* 原始作者: 张太红                                   *
* 修订日期: 2012-02-20                               *
* (c) Copyright 2006-2012. All rights reserved.      *
******************************************************
* 文件名称: db.C                                     *
* 版本编号: V1.00                                    *
* 功能描述: UBASE IO层具体实现                       *
******************************************************
* 学生学号: XXXXXXXXX                                *
* 学生姓名：YYYYYYYYY                                *
* 修订标记：对此文件没有修订                         *
* 修订日期：yyyy-mm-dd                               *
* 修订内容：无                                       *
*****************************************************/
#include <memory.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <fcntl.h>
#include <iostream>
#include <math.h>
#include <stdio.h>
#include "page.h"
#include "db.h"
#include "buf.h"


#define DBP(p)      (*(DBPage*)&p) //强制将数据页转换为头页的宏定义

/******************************************************************
* 以下为文件管理散列表的类OpenFileHashTbl的具体实现，包括：       *
* 1、构造函数：OpenFileHashTbl()                                  *
* 2、析构函数：~OpenFileHashTbl()                                 *
* 3、散列函数：hash()                                             *
* 4、散列节点插入函数：insert()                                   *
* 5、散列节点查找函数：find()                                     *
* 6、散列节点删除函数：erase()                                    *
******************************************************************/
OpenFileHashTbl::OpenFileHashTbl()
{
   HTSIZE = 113; // hack 
   // allocate an array of pointers to fleHashBuckets
   ht = new fileHashBucket* [HTSIZE];
   for(int i=0; i < HTSIZE; i++) ht[i] = NULL;
}

OpenFileHashTbl::~OpenFileHashTbl()
{
   for(int i = 0; i < HTSIZE; i++)
   {
      fileHashBucket* tmpBuf = ht[i];
      while (ht[i])
      {
         tmpBuf = ht[i];
         ht[i] = ht[i]->next;
         // blow away the file object in case someone forgot to close it
         if (tmpBuf->file != NULL) delete tmpBuf->file;
         delete tmpBuf;
      }
   }
   delete [] ht;
}

int OpenFileHashTbl::hash(const string fileName)
{
   int i,len;
   long long value;
   len =  (int) fileName.length();
   value = 0;
   for (i=0;i<len;i++)
   {
       value = 31*value + (int) fileName[i];
       //cout << "value:" << value << endl;
   }
   value  = abs(value % HTSIZE);
   return value;
}

Status OpenFileHashTbl::insert(const string fileName, File* file ) 
{
   int index = hash(fileName);
   fileHashBucket* tmpBuc = ht[index];
   while (tmpBuc)
   {
      if (tmpBuc->fname == fileName) return HASHTBLERROR;
      tmpBuc = tmpBuc->next;
   }
   tmpBuc = new fileHashBucket;
   if (!tmpBuc) return HASHTBLERROR;
   tmpBuc->fname = fileName;
   tmpBuc->file = file;
   tmpBuc->next = ht[index];
   ht[index] = tmpBuc;
 
   return OK;
}

Status OpenFileHashTbl::find(const string fileName, File*& file)
{
   int index = hash(fileName);
   fileHashBucket* tmpBuc = ht[index];
   while (tmpBuc)
   {
      if (tmpBuc->fname == fileName) 
      {
         file = tmpBuc->file;
         return OK;
      }
      tmpBuc = tmpBuc->next;
   }
   return HASHNOTFOUND;
}

Status OpenFileHashTbl::erase(const string fileName)
{
   int index = hash(fileName);
   fileHashBucket* tmpBuc = ht[index];
   fileHashBucket* prevBuc = ht[index];

   while (tmpBuc)
   {
      if (tmpBuc->fname == fileName)
      {
         if (tmpBuc == ht[index]) ht[index] = tmpBuc->next;
         else prevBuc->next = tmpBuc->next;
         tmpBuc->file = NULL;
         delete tmpBuc;
         return OK;
      } 
      else {
         prevBuc = tmpBuc;
         tmpBuc = tmpBuc->next;
      }
   }
   return HASHTBLERROR;
}

/***************************************************************
* 以下为文件对象File的具体实现，包括：                         *
* 1、构造函数：File()                                          *
* 2、析构函数：~File()                                         *
* 3、创建文件函数：create()                                    *
* 4、删除文件函数：destroy()                                   *
* 5、打开文件函数：open()                                      *
* 6、关闭文件函数：close()                                     *
* 7、分配新页函数：allocatePage�()                             *
* 8、释放空白页函数：disposePage(),将其加入空白页链表          *
* 9、读页函数：readPage()                                      *
* 10、写页函数：writePage()                                    *
* 11、取首页函数：getFirstPage（）                             *
* 12、显示所有空白页函数：listFree()                           *
***************************************************************/
File::File(const string & fname) //构造函数
{
   fileName = fname;
   openCnt = 0;
   unixFile = -1;
}

File::~File() //析构函数
{
   if (openCnt == 0) return; //文件已经正常关闭，无需再做什么

   // 否则表明文件仍然在打开，必须先将文件在缓存中已经发生变换的页写入磁盘，然后将其关闭
   // 为了保证以上操作，先把openCnt设置为1，然后调用close()方法
   openCnt = 1;
   Status status = close();
   if (status != OK)
   {
      Error error;
      error.print(status);
   }
}

Status const File::create(const string & fileName)
{
   int file;
   if ((file = ::open(fileName.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0666)) < 0)
   {
      if (errno == EEXIST)
	 return FILEEXISTS;
      else
	 return UNIXERR;
   }

   // An empty file contains just a DB header page.
   Page header;
   memset(&header, 0, sizeof header);
   DBP(header).nextFree = -1;
   DBP(header).firstPage = -1;
   DBP(header).numPages = 1;
   if (write(file, (char*)&header, sizeof header) != sizeof header)
      return UNIXERR;
   if (::close(file) < 0)
      return UNIXERR;

   return OK;
}

const Status File::destroy(const string & fileName)
{
   if (remove(fileName.c_str()) < 0)
   {
      cout << "db.destroy. unlink returned error" << "\n";
      return UNIXERR;
   }
   return OK;
}

const Status File::open()
{
   //Open file -- it will be closed in closeFile().
   if (openCnt == 0)
   {
      if ((unixFile = ::open(fileName.c_str(), O_RDWR)) < 0)
         return UNIXERR;

      // Store file info in open files table.
      openCnt = 1;
   }
   else
      openCnt++;
   return OK;
}

const Status File::close()
{
   if (openCnt <= 0)
      return FILENOTOPEN;

   openCnt--;

   // File actually closed only when open count goes to zero.
   if (openCnt == 0) {
      if (bufMgr)
         bufMgr->flushFile(this);

      if (::close(unixFile) < 0)
         return UNIXERR;
   }
   return OK;
}


// Allocate a page either from a free list (list of pages which
// were previously disposed of), or extend file if no free pages
// are available.

const Status File::allocatePage(int& pageNo)
{
   Page header;
   Status status;

   if ((status = readPage(0, &header)) != OK) return status;

   // If free list has pages on it, take one from there
   // and adjust free list accordingly.
   if (DBP(header).nextFree != -1)
   {
      // free list exists?
      // Return first page on free list to the caller,
      // adjust free list accordingly.
      pageNo = DBP(header).nextFree;
      Page firstFree;
      if ((status = readPage(pageNo, &firstFree)) != OK) return status;
      DBP(header).nextFree = DBP(firstFree).nextFree;
   }
   else
   {  
      // no free list, have to extend file
      // Extend file -- the current number of pages will be
      // the page number of the page to be returned.
      pageNo = DBP(header).numPages;
      Page newPage;
      memset(&newPage, 0, sizeof newPage);
      if ((status = writePage(pageNo, &newPage)) != OK) return status;
      DBP(header).numPages++;
      if (DBP(header).firstPage == -1)    // first user page in file?
         DBP(header).firstPage = pageNo;
   }

   if ((status = writePage(0, &header)) != OK) return status;
  
#ifdef DEBUGFREE
   listFree();
#endif

   return OK;
}


// Deallocate a page from file. The page will be put on a free
// list and returned back to the caller upon a subsequent
// allocPage() call.
const Status File::disposePage(const int pageNo)
{
   if (pageNo < 1)  return BADPAGENO;

   Page header;
   Status status;

   if ((status = readPage(0, &header)) != OK)  return status;

   // The first user-allocated page in the file cannot be
   // disposed of. The File layer has no knowledge of what
   // is the next page in the file and hence would not be
   // able to adjust the firstPage field in file header.
   if (DBP(header).firstPage == pageNo || pageNo >= DBP(header).numPages) return BADPAGENO;

   // Deallocate page by attaching it to the free list.
   Page away;
   if ((status = readPage(pageNo, &away)) != OK) return status;
   memset(&away, 0, sizeof away);
   DBP(away).nextFree = DBP(header).nextFree;
   DBP(header).nextFree = pageNo;

   if ((status = writePage(pageNo, &away)) != OK) return status;
   if ((status = writePage(0, &header)) != OK) return status;

#ifdef DEBUGFREE
   listFree();
#endif

   return OK;
}

// Read a page from file, check parameters for validity.
const Status File::readPage(const int pageNo, Page* pagePtr) const
{
   if (!pagePtr) return BADPAGEPTR;
   if (pageNo < 0) return BADPAGENO;
   if (lseek(unixFile, pageNo * sizeof(Page), SEEK_SET) == -1) return UNIXERR;

   int nbytes = read(unixFile, (char*)pagePtr, sizeof(Page));

#ifdef DEBUGIO
   cerr << "%%  File " << (int)this << ": read bytes ";
   cerr << pageNo * sizeof(Page) << ":+" << nbytes << endl;
   cerr << "%%  ";
   for(int i = 0; i < 10; i++)
      cerr << *((int*)pagePtr + i) << " ";
   cerr << endl;
#endif

   if (nbytes != sizeof(Page))  return UNIXERR;
   else return OK;
}


// Write a page to file, check parameters for validity.

const Status File::writePage(const int pageNo, const Page *pagePtr)
{
   if (!pagePtr) return BADPAGEPTR;
   if (pageNo < 0) return BADPAGENO;
   if (lseek(unixFile, pageNo * sizeof(Page), SEEK_SET) == -1) return UNIXERR;
   int nbytes = write(unixFile, (char*)pagePtr, sizeof(Page));

#ifdef DEBUGIO
   cerr << "%%  File " << (int)this << ": wrote bytes ";
   cerr << pageNo * sizeof(Page) << ":+" << nbytes << endl;
   cerr << "%%  ";
   for(int i = 0; i < 10; i++)
      cerr << *((int*)pagePtr + i) << " ";
   cerr << endl;
#endif

   if (nbytes != sizeof(Page)) return UNIXERR;
   else return OK;
}


// Return the number of the first page in file. It is stored
// on the file's header page (field firstPage).
const Status File::getFirstPage(int& pageNo) const
{
   Page header;
   Status status;

   if ((status = readPage(0, &header)) != OK) return status;

   pageNo = DBP(header).firstPage;
   return OK;
}

#ifdef DEBUGFREE

// Print out the page numbers on the free list. For debugging only.

void File::listFree()
{
   cerr << "%%  File " << (int)this << " free pages:";
   int pageNo = 0;
   for(int i = 0; i < 10; i++)
   {
      Page page;
      if (readPage(pageNo, &page) != OK) break;
      pageNo = DBP(page).nextFree;
      cerr << " " << pageNo;
      if (pageNo == -1) break;
   }
   cerr << endl;
}
#endif

/******************************************************************
* 以下为数据库类DB的具体实现，包括：                              *
* 1、构造函数：DB()                                               *
* 2、析构函数：~DB()                                              *
* 3、创建文件函数：createFile()                                   *
* 4、删除文件函数：destroyFile()                                  *
* 5、打开文件函数：openFile()                                     *
* 6、关闭文件函数：closeFile()                                    *
******************************************************************/
// Construct a DB object which keeps track of creating, opening, and
// closing files.

DB::DB()
{
   // Check that DB header page data fits on a regular data page.

   if (sizeof(DBPage) >= sizeof(Page))
   {
      cerr << "sizeof(DBPage) cannot exceed sizeof(Page): "<< sizeof(DBPage) << " " << sizeof(Page) << endl;
      exit(1);
   }
}


// Destroy DB object. 

DB::~DB()
{
   // this could leave some open files open.
   // need to fix this by iterating through the hash table deleting each open file
}


  
// Create a database file.

const Status DB::createFile(const string &fileName) 
{
   File*  file;
   if (fileName.empty())
      return BADFILE;

   // First check if the file has already been opened
   if (openFiles.find(fileName, file) == OK) return FILEEXISTS;

   // Do the actual work
   return File::create(fileName);
}


// Delete a database file.
const Status DB::destroyFile(const string & fileName) 
{
   File* file;

   if (fileName.empty()) return BADFILE;

   // Make sure file is not open currently.
   if (openFiles.find(fileName, file) == OK) return FILEOPEN;
  
   // Do the actual work
   return File::destroy(fileName);
}


// Open a database file. If file already open, increment open count,
// otherwise find a vacant slot in the open files table and store
// file info there.
const Status DB::openFile(const string & fileName, File*& filePtr)
{
   Status status;
   File* file;

   if (fileName.empty()) return BADFILE;

   // Check if file already open. 
   if (openFiles.find(fileName, file) == OK) 
   {
      // file is already open, call open again on the file object
      // to increment it's open count.
      status = file->open();
      filePtr = file;
   }
   else
   {
      // file is not already open
      // Otherwise create a new file object and open it
      filePtr = new File(fileName);
      status = filePtr->open();

      if (status != OK)
      {
         delete filePtr;
         return status;
      }
      // Insert into the mapping table
      status = openFiles.insert(fileName, filePtr);
   }
   return status;
}


// Close a database file. Get file info from open files table,
// call Unix close() only if open count now goes to zero.
const Status DB::closeFile(File* file)
{
   if (!file) return BADFILEPTR;

   // Close the file
   file->close();

   // If there are no remaining references to the file, then we should delete
   // the file object and remove it from the openFiles hash table.

   if (file->openCnt == 0)
   {
      if (openFiles.erase(file->fileName) != OK) return BADFILEPTR;
      delete file;
   }
   return OK;
}
