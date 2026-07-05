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

#include <ftpd.h>
#include <startnet.h>

#include "effs_std.h"

#define FTP_BUFFER_SIZE (32 * 1024)
#define MAX_STRING 128

static char FTP_buffer[FTP_BUFFER_SIZE] __attribute__((aligned(16)));
static const char mstr[12][4] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

void fs_displaytime()
{
    unsigned short ctime = fs_gettime();
    unsigned short cdate = fs_getdate();

    printf("%2.2d:%2.2d:%2.2d |", ((ctime & FS_CTIME_HOUR_MASK) >> FS_CTIME_HOUR_SHIFT),
            ((ctime & FS_CTIME_MIN_MASK) >> FS_CTIME_MIN_SHIFT), 2 * (ctime & FS_CTIME_SEC_MASK));

    printf("%2.2d/%2.2d/%4d   |", ((cdate & FS_CDATE_MONTH_MASK) >> FS_CDATE_MONTH_SHIFT), (cdate & FS_CDATE_DAY_MASK),
            (1980 + ((cdate & FS_CDATE_YEAR_MASK) >> FS_CDATE_YEAR_SHIFT)));
    printf("\r\n");
}

/*
 *  settimedate
 *
 *  Set the date
 */
void settimedate(FS_FIND *f)
{
    printf("Setting %s date and time to: ", f->filename);
    fs_displaytime();

    int nret = fs_settimedate(f->filename, fs_gettime(), fs_getdate());
    if (nret == FS_NO_ERROR) { printf("Time stamping successful\r\n"); }
    else
    {
        printf("Time stamping failed: %d\r\n", nret);
    }
}

/*
 *  gettimedate
 *
 *  This function displays information in MTTTY of subdirectories
 *  and files in a current directory.  The information displayed is: the
 *  filename, modified time stamp, modified date stamp, and file size.
 */
void gettimedate(FS_FIND *f)
{
    unsigned short t, d;

    printf("Display time in gettimedate(): ");
    fs_displaytime();

    int nret = fs_gettimedate(f->filename, &t, &d);
    if (nret == FS_NO_ERROR)
    {
        printf("%15s   |", f->filename);
        printf("%2.2d:%2.2d:%2.2d   |", ((t & 0xF800) >> 11), ((t & 0x07E0) >> 5), 2 * (t & 0x001F));
        printf("%2.2d/%2.2d/%4d   |", ((d & 0x01E0) >> 5), (d & 0x001F), (1980 + ((d & 0xFE00) >> 9)));
        printf("%9ld Bytes\r\n", f->len);
    }
    else
    {
        printf("Time stamp retrieval failed: %d\r\n", nret);
    }
}

/*
 *  getdatestring
 *
 *  Generate date string.
 */
static void getdatestring(FS_FIND *f, char *tmp)
{
    // Converts file time and date stamp to a value that can be interpreted by
    // the user.
    // unsigned short sec = 2 * ( ( f->ctime ) & FS_CTIME_SEC_MASK );
    unsigned short minute = ((f->ctime) & FS_CTIME_MIN_MASK) >> FS_CTIME_MIN_SHIFT;
    unsigned short hour = ((f->ctime) & FS_CTIME_HOUR_MASK) >> FS_CTIME_HOUR_SHIFT;
    unsigned short day = (f->cdate) & FS_CDATE_DAY_MASK;
    unsigned short month = ((f->cdate) & FS_CDATE_MONTH_MASK) >> FS_CDATE_MONTH_SHIFT;
    unsigned short year = 1980 + (((f->cdate) & FS_CDATE_YEAR_MASK) >> FS_CDATE_YEAR_SHIFT);

    // For FTP file properties: If the current year matches the year stamp of the
    // associated file, then the hour and minutes are displayed.  Otherwise, the
    // year is used in place of hour and minutes.
    if ((1980 + (((fs_getdate()) & FS_CDATE_YEAR_MASK) >> FS_CDATE_YEAR_SHIFT)) == year)
    {
        siprintf(tmp, "%3s %2d %2.2d:%2.2d", mstr[month - 1], day, hour, minute);
    }
    else
    {
        siprintf(tmp, "%3s %2d  %4d", mstr[month - 1], day, year);
    }

    printf("getdatestring(): %s\r\n", tmp);
}

/*
 *  getdirstring
 *
 *  Generate dir entry string.
 */
static void getdirstring(FS_FIND *f, char *dst)
{
    char tmp[32];

    if ((f->attr) & FS_ATTR_DIR) { dst[0] = 'd'; }
    else
    {
        dst[0] = '-';
    }

    dst[1] = 0;
    strcat(dst, "rw-rw-rw-   1 none");
    strcat(dst, " ");

    sniprintf(tmp, 32, "%9ld", f->len);
    strcat(dst, tmp);
    strcat(dst, " ");

    getdatestring(f, tmp);
    strcat(dst, tmp);
    strcat(dst, " ");

    strcat(dst, f->filename);
}

/*
 *  FTPDSessionStart
 *
 *  Start FTP session.
 */
void *FTPDSessionStart(const char *user, const char *passwd, const IPADDR4 hi_ip)
{
    fs_chdrive(DEFAULT_NOR_DRV);
    fs_chdir("/");
    return ((void *)1);
}

/*
 *  FTPDSessionEnd
 *
 *  Finish FTP session.
 */
void FTPDSessionEnd(void *pSession) {}

/*
 *  FTPD_DirectoryExists
 *
 *  Check for a directory.
 */
int FTPD_DirectoryExists(const char *full_directory, void *pSession)
{
    if (*full_directory == 0) { return (FTPD_OK); }

    fs_chdir("/");

    if (fs_chdir((char *)full_directory)) { return (FTPD_FAIL); }
    else
    {
        return (FTPD_OK);
    }

    return (FTPD_OK);
}

/*
 *  FTPD_CreateSubDirectory
 *
 *  Create directory.
 */
int FTPD_CreateSubDirectory(const char *current_directory, const char *new_dir, void *pSession)
{
    int rc;

    fs_chdir("/");

    if (*current_directory)
    {
        rc = fs_chdir((char *)current_directory);

        if (rc) { return (FTPD_FAIL); }
    }

    rc = fs_mkdir((char *)new_dir);

    if (rc == 0) { return (FTPD_OK); }

    return (FTPD_FAIL);
}

/*
 *  FTPD_DeleteSubDirectory
 *
 *  Delete directory.
 */
int FTPD_DeleteSubDirectory(const char *current_directory, const char *sub_dir, void *pSession)
{
    int rc;

    fs_chdir("/");

    if (*current_directory)
    {
        rc = fs_chdir((char *)current_directory);

        if (rc) { return (FTPD_FAIL); }
    }

    rc = fs_rmdir((char *)sub_dir);

    if (rc == 0) { return (FTPD_OK); }

    return (FTPD_FAIL);
}

/*
 *  FTPD_ListSubDirectories
 *
 *  List directories.
 */
int FTPD_ListSubDirectories(const char *current_directory, void *pSession, FTPDCallBackReportFunct *pFunc, int socket)
{
    FS_FIND find;
    long rc;
    char s[128];

    fs_chdir("/");

    if (*current_directory)
    {
        if (fs_chdir((char *)current_directory)) { return (FTPD_FAIL); }
    }

    rc = fs_findfirst("*", &find);

    if (!rc)
    {
        do
        {
            if (find.attr & FS_ATTR_DIR)
            {
                getdirstring(&find, s);
                pFunc(socket, s);
            }
        } while (!fs_findnext(&find));
    }

    return (FTPD_OK);
}

/*
 *  FTPD_FileExists
 *
 *  Check for a file.
 */
int FTPD_FileExists(const char *full_directory, const char *file_name, void *pSession)
{
    FS_FILE *t;

    if (strcmp(file_name, "_format") == 0 || strcmp(file_name, "_hformat") == 0 || strcmp(file_name, "_nor") == 0 || strcmp(file_name, "_nor1") == 0) { return (FTPD_OK); }

    fs_chdir("/");

    if (*full_directory)
    {
        if (fs_chdir((char *)full_directory)) { return (FTPD_FAIL); }
    }

    t = fs_open((char *)file_name, "r");

    if (t)
    {
        fs_close(t);
        return (FTPD_OK);
    }

    return (FTPD_FAIL);
}

/*
 *  FTPD_SendFileToClient
 *
 *  Retrieve function for FTP (get).
 */
int FTPD_SendFileToClient(const char *full_directory, const char *file_name, void *pSession, int fd)
{
    FS_FILE *rfile;   // Actual opened file

    if (strcmp(file_name, "_nor") == 0)
    {
        printf("Change to NOR.\r\n");
        fs_chdrive(NOR_DRV_NUM);
        return (FTPD_FAIL);
    }

    if (strcmp(file_name, "_nor1") == 0)
    {
        printf("Change to NOR partition 1.\r\n");
        fs_chdrive(NOR_DRV_PART_1);
        return (FTPD_FAIL);
    }

    if (strcmp(file_name, "_format") == 0)
    {
#if (defined SOMRT1061 || defined MODRT1171)
        printf("Format disabled: app image in file system\r\n");
#else
        int rc = 0;
        int drvNum = fs_getdrive();
        if(drvNum >= NOR_DRV_NUM)
        {
            printf("Formatting drive %d...\r\n", drvNum);
            rc = fs_format(drvNum);
        }
#endif
        return (FTPD_FAIL);
    }

    fs_chdir("/");

    if (*full_directory)
    {
        if (fs_chdir((char *)full_directory)) { return (FTPD_FAIL); }
    }

    /* If file is not open, then try to open it. */
    rfile = fs_open((char *)file_name, "r");

    /* Return with error if not successful. */
    if (!rfile) { return (FTPD_FAIL); }

    while (!fs_eof(rfile))
    {
        long bytes_read = 0;
        int retry = 0;

        do
        {
            bytes_read = fs_read(FTP_buffer, 1, FTP_BUFFER_SIZE, rfile);   // Read from file

            // retry if busy
            if ((bytes_read == 0) && (!fs_eof(rfile)))
            {
                // int error = fs_getlasterror();
                // printf("*** getlasterror: %d, bytes read = %ld ***\r\n", error, bytes_read );
                OSTimeDly(TICKS_PER_SECOND / 4);
                retry++;
            }
            else
                break;
        } while (retry < 10);

        writeall(fd, FTP_buffer, bytes_read);
    }

    fs_close(rfile);   // Close the file
    return (FTPD_OK);
}

/*
 *  FTPD_AbleToCreateFile
 *
 *  Check if it is possible to create a file.
 */
int FTPD_AbleToCreateFile(const char *full_directory, const char *file_name, void *pSession)
{
    return (FTPD_OK);
}

/*
 *  FTPD_GetFileFromClient
 *
 *  Write file to drive.
 */
int FTPD_GetFileFromClient(const char *full_directory, const char *file_name, void *pSession, int fd)
{
    FS_FILE *wfile = 0;   // Actual opened file
    FS_FIND find;
    int FtpBufIndex, bytes_read;
    int retry = 0;
    long bytes_written = 0;

    fs_chdir("/");

    if (*full_directory)
    {
        if (fs_chdir((char *)full_directory)) { return (FTPD_FAIL); }
    }

    /* Only accept file names of size 12 (8+1+3). */
    // if ( strlen( ( char * ) file_name ) > 12 ) return( FTPD_FAIL );

    wfile = fs_open((char *)file_name, "w");   // Open it for write

    int rc = fs_findfirst(file_name, &find);
    if (rc == 0)
    {
        if (!(find.attr & FS_ATTR_DIR)) { settimedate(&find); }
    }
    else
    {
        printf("fs_findfirst failed\r\n");
    }

    /* Return error if not successful. */
    if (!wfile) { return (FTPD_FAIL); }

    FtpBufIndex = 0;   // position in buffer
    /* This loop will read the file data from the client, accumulate
     * data until FTP_BUFFER_SIZE is reached, then write the data to
     * flash in one write.
     */
    do
    {
        bytes_read = ReadWithTimeout(fd, FTP_buffer + FtpBufIndex, FTP_BUFFER_SIZE - FtpBufIndex, TICKS_PER_SECOND * 20);
        printf("Bytes read: %d\r\n", bytes_read);

        if (bytes_read > 0)
        {
            FtpBufIndex += bytes_read;
            printf("FtpBufIndex: %d\r\n", FtpBufIndex);
        }

        if (FtpBufIndex == FTP_BUFFER_SIZE)
        {
            bytes_written = 0;
            retry = 0;
            do
            {
                //  buffer, size of item, number of items, file handle
                bytes_written = fs_write(FTP_buffer, 1, FtpBufIndex, wfile);
                printf("bytes_written: %ld\r\n", bytes_written);

                if (bytes_written < FtpBufIndex)
                {
                    printf("Retry: %d\r\n", retry);
                    OSTimeDly(TICKS_PER_SECOND / 4);
                    retry++;
                }
            } while ((bytes_written < FtpBufIndex) && (retry < 10));

            FtpBufIndex = 0;
        }
    } while ((bytes_read > 0) && (retry < 10));

    if (retry >= 10)
    {
        printf("Error - could not write file: %s\r\n", file_name);
        do
        {
            bytes_read = ReadWithTimeout(fd, FTP_buffer, FTP_BUFFER_SIZE, TICKS_PER_SECOND * 2);
            // printf("Bytes read: %d\r\n", bytes_read );
        } while (bytes_read > 0);

        fs_close(wfile);
        fs_delete(file_name);
        return (FTPD_FAIL);
    }

    printf("FtpBufIndex: %d\r\n", FtpBufIndex);
    if (FtpBufIndex)
    {
        bytes_written = fs_write(FTP_buffer, 1, FtpBufIndex, wfile);
        printf("wrote %ld bytes\r\n", bytes_written);
        if (bytes_written < FtpBufIndex)
        {
            printf("Error - could not write file: %s\r\n", file_name);
            do
            {
                bytes_read = ReadWithTimeout(fd, FTP_buffer, FTP_BUFFER_SIZE, TICKS_PER_SECOND * 2);
                // printf("Bytes read: %d\r\n", bytes_read );
            } while (bytes_read > 0);
            fs_close(wfile);
            fs_delete(file_name);
            return (FTPD_FAIL);
        }
    }

    fs_close(wfile);
    return (FTPD_OK);
}

/*
 *  FTPD_DeleteFile
 *
 *  Delete a file.
 */
int FTPD_DeleteFile(const char *current_directory, const char *file_name, void *pSession)
{
    fs_chdir("/");

    if (*current_directory)
    {
        if (fs_chdir((char *)current_directory)) { return (FTPD_FAIL); }
    }

    if (fs_delete((char *)file_name)) { return (FTPD_FAIL); }

    return (FTPD_OK);
}

/*
 *  FTPD_ListFile
 *
 *  Get file list.
 */
int FTPD_ListFile(const char *current_directory, void *pSession, FTPDCallBackReportFunct *pFunc, int socket)
{
    FS_FIND find;
    char s[128];
    int rc;

    fs_chdir("/");

    if (*current_directory)
    {
        if (fs_chdir((char *)current_directory)) { return (FTPD_FAIL); }
    }

    rc = fs_findfirst("*", &find);

    if (rc == 0)
    {
        printf("\r\nDisplaying time/date information\r\n");
        do
        {
            if (!(find.attr & FS_ATTR_DIR))
            {
                getdirstring(&find, s);
                gettimedate(&find);
                pFunc(socket, s);
            }
        } while (!fs_findnext(&find));
        printf("\r\n");
    }
    // else
    //{
    //   sniprintf( s, 128, "Card error: %d", rc );
    //   pFunc( socket, s );
    //}

    return (FTPD_OK);
}

/*
 *  FTPD_Rename
 *
 *  Rename a file
 */
int FTPD_Rename(const char *full_directory, const char *old_file_name, const char *new_file_name, void *pSession)
{
    fs_chdir("/");

    if (*full_directory)
    {
        if (fs_chdir((char *)full_directory)) { return (FTPD_FAIL); }
    }

    if (fs_rename(old_file_name, new_file_name)) { return FTPD_FAIL; }

    return FTPD_OK;
}

/* Reply to a file size request */
int FTPD_GetFileSize(const char *full_directory, const char *file_name)
{
    fs_chdir("/");

    uint32_t len = strlen(file_name);
    if ((file_name[len - 1] != '/') && (FTPD_FileExists(full_directory, file_name, nullptr) == FTPD_FAIL))
    {
        return FTPD_FILE_SIZE_NOSUCH_FILE;
    }

    long fileLength = fs_filelength(file_name);

    if (*full_directory)
    {
        if (fs_chdir((char *)full_directory)) { return FTPD_FILE_SIZE_NOSUCH_FILE; }
    }

    if (file_name[len - 1] == '/') { fileLength = 0; }
    else if (fileLength < 0)
    {
        return FTPD_FILE_SIZE_NOSUCH_FILE;
    }

    return fileLength;
}
