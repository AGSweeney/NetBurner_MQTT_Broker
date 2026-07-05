/* Revision: 3.5.8 */

/******************************************************************************
* Copyright 1998-2024 NetBurner, Inc.  ALL RIGHTS RESERVED
*
*    Permission is hereby granted to purchasers of NetBurner Hardware to use or
*    modify this computer program for any use as long as the resultant program
*    is only executed on NetBurner provided hardware.
*
*    No other rights to use this program or its derivatives in part or in
*    whole are granted.
*
*    It may be possible to license this or other NetBurner software for use on
*    non-NetBurner Hardware. Contact sales@Netburner.com for more information.
*
*    NetBurner makes no representation or warranties with respect to the
*    performance of this computer program, and specifically disclaims any
*    responsibility for any damages, special or consequential, connected with
*    the use of this program.
*
* NetBurner
* 16855 W Bernardo Dr
* San Diego, CA 92127
* www.netburner.com
******************************************************************************/

#include <predef.h>
#include <basictypes.h>
#include <ctype.h>
#include <stdio.h>
#include <utils.h>

#include "FileSystemUtils.h"
#include "effs_std.h"

extern "C"
{
    ;
}

char driveType[20];

char EffsErrorCode[][80] = {
    "F_NO_ERROR",                // 0
    "F_ERR_INVALIDDRIVE",        // 1
    "F_ERR_NOTFORMATTED",        // 2
    "F_ERR_INVALIDDIR",          // 3
    "F_ERR_INVALIDNAME",         // 4
    "F_ERR_NOTFOUND",            // 5
    "F_ERR_DUPLICATED",          // 6
    "F_ERR_NOMOREENTRY",         // 7
    "F_ERR_NOTOPEN",             // 8
    "F_ERR_EOF",                 // 9
    "F_ERR_RESERVED",            // 10
    "F_ERR_NOTUSEABLE",          // 11
    "F_ERR_LOCKED",              // 12
    "F_ERR_ACCESSDENIED",        // 13
    "F_ERR_NOTEMPTY",            // 14
    "F_ERR_INITFUNC",            // 15
    "F_ERR_CARDREMOVED",         // 16
    "F_ERR_ONDRIVE",             // 17
    "F_ERR_INVALIDSECTOR",       // 18
    "F_ERR_READ",                // 19
    "F_ERR_WRITE",               // 20
    "F_ERR_INVALIDMEDIA",        // 21
    "F_ERR_BUSY",                // 22
    "F_ERR_WRITEPROTECT",        // 23
    "F_ERR_INVFATTYPE",          // 24
    "F_ERR_MEDIATOOSMALL",       // 25
    "F_ERR_MEDIATOOLARGE",       // 26
    "F_ERR_NOTSUPPSECTORSIZE",   // 27
    "F_ERR_DELFUNC",             // 28
    "F_ERR_MOUNTED",             // 29
    "F_ERR_TOOLONGNAME",         // 30
    "F_ERR_NOTFORREAD",          // 31
    "F_ERR_DELFUNC",             // 32
    "F_ERR_ALLOCATION",          // 33
    "F_ERR_INVALIDPOS",          // 34
    "F_ERR_NOMORETASK",          // 35
    "F_ERR_NOTAVAILABLE",        // 36
    "F_ERR_TASKNOTFOUND",        // 37
    "F_ERR_UNUSABLE"             // 38
};

/*-------------------------------------------------------------------
  DisplayEffsErrorCode()
 -------------------------------------------------------------------*/
void DisplayEffsErrorCode(int code)
{
    if (code <= MAX_EFFS_ERRORCODE) { printf("%s\r\n", EffsErrorCode[code]); }
    else
    {
        printf("Unknown EFFS error code [%d]\r\n", code);
    }
}

#include <nbrtos.h>
/*-------------------------------------------------------------------
uint8_t FormatSD()
 -------------------------------------------------------------------*/
uint8_t FormatEffsStdFlash(int drvNum)
{
    /*
     * fs_format() is disabled on the RT platforms (SOMRT1061/MODRT1171) because the
     * application image lives in the file system -- formatting would erase the app.
     * If you do not target an RT platform, this #if/#else can be reduced to just the
     * #else (format) branch.
     */
#if (defined SOMRT1061 || defined MODRT1171)
    printf("Format disabled: app image in file system\r\n");
    return 1;
#else
    int rv;
    printf("Formatting File System ... ");
    rv = fs_format(drvNum);
    if (rv != FS_NO_ERROR)
    {
        printf("*** Error in fs_format(): ");
        DisplayEffsErrorCode(rv);
    }
    else
        printf("complete\r\n");
    return rv;
#endif
}

/*-------------------------------------------------------------------
  DisplayEffsSpaceStats() - Display space used, total and bad
 -------------------------------------------------------------------*/
uint8_t DisplayEffsSpaceStats(int drvNum)
{
    FS_SPACE space;
    volatile int rv;
    printf("Retrieving flash usage...\r\n");
    rv = fs_getfreespace(drvNum, &space);

    if (rv == FS_NO_ERROR)
    {
        printf("EFFS Flash memory usage (bytes):\r\n");
        printf("%lu total, %lu free, %lu used, %lu bad\r\n", space.total, space.free, space.used, space.bad);
    }
    else
    {
        printf("\r\n*** Error in fs_getfreepace(): ");
        DisplayEffsErrorCode(rv);
    }

    return rv;
}

extern void gettimedate(FS_FIND *f);

/*-------------------------------------------------------------------
  DumpDir() - Displays a list of all directories and files in the
  file system.
 -------------------------------------------------------------------*/
uint8_t DumpDir()
{
    FS_FIND finder;   // location to store the information retrieved

    /* Find first file or subdirectory in specified directory. First call the
       fs_findfirst function, and if file was found get the next file with
       fs_findnext function. Files with the system attribute set will be ignored.
       Note: If fs_findfirst() is called with "*.*" and the current directory is
       not the root directory, the first entry found will be "." - the current
       directory.
    */
    volatile int rc = fs_findfirst("*", &finder);
    if (rc == FS_NO_ERROR)   // found a file or directory
    {
        do
        {
            if ((finder.attr & FS_ATTR_DIR))
            {
                printf("Found Directory [");
                if (*finder.findfsname.path) printf("%s/", finder.findfsname.path);
                printf("%s]\r\n", finder.filename);

                if (finder.filename[0] != '.')
                {
                    fs_chdir(finder.filename);
                    DumpDir();
                    fs_chdir("..");
                }
            }
            else
            {
                printf("Found File [%s/%s] : %lu Bytes\r\n", finder.findfsname.path, finder.filename, finder.len);
                gettimedate(&finder);
            }
        } while (!fs_findnext(&finder));
    }

    return rc;
}

/*
typedef struct
{
    char filename[FN_MAXPATH];  // file name+ext
    char name[F_MAXNAME];       // file name
    char ext[F_MAXEXT];         // file extension
    char attr;                     // attribute of the file

    unsigned short ctime;       // creation time
    unsigned short cdate;       // creation date
    unsigned long filesize;     // length of file

    unsigned long cluster;      // current file starting position
    F_NAME findfsname;         // find properties
    F_POS pos;                     // position of the current list
} FN_FIND;
*/

/*-------------------------------------------------------------------
  uint8_t DeleteFile( char* );  - Deletes File and prints status, returns 0 for success
 -------------------------------------------------------------------*/
uint8_t DeleteFile(char *pFileName)
{
    volatile int rv;
    rv = fs_delete(pFileName);
    if (rv != FS_NO_ERROR)
    {
        printf("\r\n*** Error in fs_delete( %s )\r\n", pFileName);
        DisplayEffsErrorCode(rv);
    }
    return rv;
}

/*-------------------------------------------------------------------
 uint32_t WriteFile( uint8_t* pDataToWrite, char* pFileName, uint32_t NumBytes );
  - Open, Writes, Closes File and prints status, returns 0 for success

-------------------------------------------------------------------*/
uint32_t WriteFile(uint8_t *pDataToWrite, char *pFileName, uint32_t NumBytes)
{
    uint32_t rvW, rvC;
    FS_FILE *fp = fs_open(pFileName, "w+");
    if (fp)
    {
        rvW = fs_write(pDataToWrite, 1, NumBytes, fp);
        if (rvW != NumBytes) printf("\r\n*** Error in fs_write(%s): %lu out of %lu bytes writte\r\n", pFileName, rvW, NumBytes);

        rvC = fs_close(fp);   // Close a previously opened file of type FS_FILE
        if (rvC != FS_NO_ERROR)
        {
            printf("*** Error in fs_close(%s): ", pFileName);
            DisplayEffsErrorCode(rvC);
            return 0;
        }
    }
    else
    {
        printf("Failed to open file %s\r\n", pFileName);
        return 0;
    }

    return rvW;
}

/*-------------------------------------------------------------------
 uint32_t AppendFile( uint8_t* pDataToWrite, char* pFileName, uint32_t NumBytes );
  - Open, Writes stating at end of data in file, Closes File and prints status, returns 0 for success

-------------------------------------------------------------------*/
uint32_t AppendFile(uint8_t *pDataToWrite, char *pFileName, uint32_t NumBytes)
{
    uint32_t rvA, rvC;
    FS_FILE *fp = fs_open(pFileName, "a+");
    if (fp)
    {
        rvA = fs_write(pDataToWrite, 1, NumBytes, fp);
        if (rvA != NumBytes) printf("\r\n*** Error in fs_write(%s): %lu out of %lu bytes written\r\n", pFileName, rvA, NumBytes);

        rvC = fs_close(fp);   // Close a previously opened file of type FS_FILE
        if (rvC != FS_NO_ERROR)
        {
            printf("*** Error in fs_close(%s): ", pFileName);
            DisplayEffsErrorCode(rvC);
            return 0;
        }
    }
    else
    {
        printf("Failed to open file %s\r\n", pFileName);
        return 0;
    }

    return rvA;
}

/*-------------------------------------------------------------------
 uint32_t ReadFile( uint8_t* pReadBuffer, char* pFileName, uint32_t NumBytes );
  - Open, Writes, Closes File and prints status, returns 0 for success

-------------------------------------------------------------------*/
uint32_t ReadFile(uint8_t *pReadBuffer, char *pFileName, uint32_t NumBytes)
{
    uint32_t rvR, rvC;
    FS_FILE *fp = fs_open(pFileName, "r");
    if (fp)
    {
        rvR = (uint32_t)fs_read(pReadBuffer, 1, NumBytes, fp);
        // if( NumBytes != rvR )
        //    printf( "*** Warning in ReadFile(%s): %lu out of %lu bytes read\r\n", pFileName, rvR, NumBytes );

        rvC = fs_close(fp);   // Close a previously opened file of type FS_FILE
        if (rvC != FS_NO_ERROR)
        {
            printf("*** Error in fs_close(%s): ", pFileName);
            DisplayEffsErrorCode(rvC);
            return 0;
        }
    }
    else
    {
        printf("Failed to open file %s\r\n", pFileName);
        return 0;
    }
    return rvR;
}

extern unsigned short fs_getdate();
extern unsigned short fs_gettime();

/*-------------------------------------------------------------------
  ReadWriteTest() - This function will read and write files/data to
  demonstrate basic file system operation.
 -------------------------------------------------------------------*/
void ReadWriteTest()
{
    /* Create a test file
       The fs_open() function opens a file for reading or writing. The following
       modes are allowed to open:
          "r"   Open existing file for reading. The stream is positioned at the
                beginning of the file.
          "r+"  Open existing file for reading and writing. The stream is
                positioned at the beginning of the file.
          "w"   Truncate file to zero length or create file for writing. The
                stream is positioned at the beginning of the file.
          "w+"  Open a file for reading and writing. The file is created if it
                does not exist, otherwise it is truncated. The stream is
                positioned at the beginning of the file.
          "a"   Open for appending (writing to end of file). The file is created
                if it does not exist. The stream is positioned at the end of the
                file.
          "a+"  Open for reading and appending (writing to end of file). The file
                is created if it does not exist. The stream is positioned at the
                end of the file.
       Note: There is no text mode. The system assumes all files to be accessed in
       binary mode only.
    */
    char *FileName = "TestFile.txt";   // 8.3 file names supported by default
    printf("\r\nCreating test file: %s\r\n", FileName);
    FS_FILE *fp = fs_open(FileName, "w+");
    if (fp)
    {
        for (int i = 0; i < 5; i++)
        {
#define WRITE_BUFSIZE 80
            char write_buf[WRITE_BUFSIZE];
            sniprintf(write_buf, WRITE_BUFSIZE, "Hello World %d\r\n", i);
            /* fs_write( const void *buffer,  // pointer to data to be written
               long size,           // size of items to be written
               long size size_st,   // number of items to be written
               FS_FILE )             // handle of target file

               Returns the number of items written.
            */
            int n = fs_write(write_buf, 1, strlen(write_buf), fp);
            printf("Wrote %d bytes: %s", n, write_buf);
        }

        // Read the data in the test file
        printf("\r\nRewinding file\r\n");
        int rv = fs_rewind(fp);   // set current file pointer to start of file
        if (rv != FS_NO_ERROR)
        {
            printf("\r\n*** Error in fs_close(): fp = %d\r\n", fp);
            DisplayEffsErrorCode(rv);
        }
        else
        {
            while (!fs_eof(fp))
            {
/* Read bytes from the current position in the target file. File has
   to be opened with �r�, "r+", "w+" or "a+".
   fs_read ( const void *buffer,  // pointer to buffer to store data
            long size,           // size of items to be read
            long size size_st,   // number of items to be read
            FS_FILE )             // handle of target file
   Returns the number of items read.
*/
#define READ_BUFSIZE 80
                char read_buf[READ_BUFSIZE];
                int n = fs_read(read_buf, 1, READ_BUFSIZE - 1, fp);
                read_buf[n] = '\0';   // terminate string
                printf("Read %d bytes:\r\n%s\r\n", n, read_buf);
            }

            printf("Closing file %s\r\n\r\n", FileName);
            rv = fs_close(fp);   // Close a previously opened file of type FS_FILE
            if (rv != FS_NO_ERROR)
            {
                printf("*** Error in fs_close(): ");
                DisplayEffsErrorCode(rv);
            }

            // fs_settimedate( FileName, fs_gettime(), fs_getdate());
        }
    }
    else
    {
        printf("\r\n*** Error opening file \"%s\", fp = %d\r\n", FileName, fp);
    }
}

/*-------------------------------------------------------------------
 Display a text file
 ------------------------------------------------------------------*/
void DisplayTextFile(char *FileName)
{
    printf("\r\nOpening test file for reading: %s\r\n", FileName);
    FS_FILE *fp = fs_open(FileName, "r");
    if (fp)
    {
        while (!fs_eof(fp))
        {
/* Read bytes from the current position in the target file. File has
   to be opened with �r�, "r+", "w+" or "a+".
   fs_read ( const void *buffer,  // pointer to buffer to store data
            long size,           // size of items to be read
            long size size_st,   // number of items to be read
            FS_FILE )             // handle of target file
   Returns the number of items read.
*/
#define DISP_READ_BUFSIZE 255
            char read_buf[DISP_READ_BUFSIZE];
            int n = fs_read(read_buf, 1, DISP_READ_BUFSIZE - 1, fp);
            read_buf[n] = '\0';   // terminate string
            printf("Read %d bytes:\r\n%s\r\n", n, read_buf);
        }

        printf("Closing file %s\r\n\r\n", FileName);
        int rv = fs_close(fp);   // Close a previously opened file of type FS_FILE
        if (rv != FS_NO_ERROR)
        {
            printf("*** Error in fs_close(): ");
            DisplayEffsErrorCode(rv);
        }
    }
    else
    {
        printf("\r\n*** Error opening file in DisplayTextFile() \"%s\", fp = %d\r\n", FileName, fp);
    }
}
