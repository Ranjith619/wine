/*
 * Profile functions
 *
 * Copyright 1993 Miguel de Icaza
 * Copyright 1996 Alexandre Julliard
 */

#define NO_TRANSITION_TYPES  /* This file is Win32-clean */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "windows.h"
#include "dos_fs.h"
#include "xmalloc.h"
#include "string32.h"
#include "stddebug.h"
#include "debug.h"

typedef struct tagPROFILEKEY
{
    char                  *name;
    char                  *value;
    struct tagPROFILEKEY  *next;
} PROFILEKEY;

typedef struct tagPROFILESECTION
{
    char                       *name;
    struct tagPROFILEKEY       *key;
    struct tagPROFILESECTION   *next;
} PROFILESECTION; 


typedef struct
{
    BOOL32           changed;
    PROFILESECTION  *section;
    char            *dos_name;
} PROFILE;


/* Cached profile file */
static PROFILE CurProfile = { FALSE, NULL, NULL };

/* wine.ini profile content */
static PROFILESECTION *WineProfile;

#define PROFILE_MAX_LINE_LEN   1024

/* Wine profile name in $HOME directory; must begin with slash */
static const char PROFILE_WineIniName[] = "/.winerc";

/* Check for comments in profile */
#define IS_ENTRY_COMMENT(str)  ((str)[0] == ';')

#define WINE_INI_GLOBAL ETCDIR "/wine.conf"

static LPCWSTR wininiW = NULL;

/***********************************************************************
 *           PROFILE_CopyEntry
 *
 * Copy the content of an entry into a buffer, removing quotes, and possibly
 * translating environment variables.
 */
static void PROFILE_CopyEntry( char *buffer, const char *value, int len,
                               int handle_env )
{
    char quote = '\0';
    const char *p;

    if ((*value == '\'') || (*value == '\"'))
    {
        if (value[1] && (value[strlen(value)-1] == *value)) quote = *value++;
    }

    if (!handle_env)
    {
        lstrcpyn32A( buffer, value, len );
        if (quote && (len >= strlen(value))) buffer[strlen(buffer)-1] = '\0';
        return;
    }

    for (p = value; (*p && (len > 1)); *buffer++ = *p++, len-- )
    {
        if ((*p == '$') && (p[1] == '{'))
        {
            char env_val[1024];
            const char *env_p;
            const char *p2 = strchr( p, '}' );
            if (!p2) continue;  /* ignore it */
            lstrcpyn32A(env_val, p + 2, MIN( sizeof(env_val), (int)(p2-p)-1 ));
            if ((env_p = getenv( env_val )) != NULL)
            {
                lstrcpyn32A( buffer, env_p, len );
                buffer += strlen( buffer );
                len -= strlen( buffer );
            }
            p = p2 + 1;
        }
    }
    *buffer = '\0';
}


/***********************************************************************
 *           PROFILE_Save
 *
 * Save a profile tree to a file.
 */
static void PROFILE_Save( FILE *file, PROFILESECTION *section )
{
    PROFILEKEY *key;

    for ( ; section; section = section->next)
    {
        if (section->name) fprintf( file, "[%s]\r\n", section->name );
        for (key = section->key; key; key = key->next)
        {
            fprintf( file, "%s", key->name );
            if (key->value) fprintf( file, "=%s", key->value );
            fprintf( file, "\r\n" );
        }
    }
}


/***********************************************************************
 *           PROFILE_Free
 *
 * Free a profile tree.
 */
static void PROFILE_Free( PROFILESECTION *section )
{
    PROFILESECTION *next_section;
    PROFILEKEY *key, *next_key;

    for ( ; section; section = next_section)
    {
        if (section->name) free( section->name );
        for (key = section->key; key; key = next_key)
        {
            next_key = key->next;
            if (key->name) free( key->name );
            if (key->value) free( key->value );
            free( key );
        }
        next_section = section->next;
        free( section );
    }
}


/***********************************************************************
 *           PROFILE_Load
 *
 * Load a profile tree from a file.
 */
static PROFILESECTION *PROFILE_Load( FILE *file )
{
    char buffer[PROFILE_MAX_LINE_LEN];
    char *p, *p2;
    int line = 0;
    PROFILESECTION *section, *first_section;
    PROFILESECTION **prev_section;
    PROFILEKEY *key, **prev_key;

    first_section = (PROFILESECTION *)xmalloc( sizeof(*section) );
    first_section->name = NULL;
    first_section->key  = NULL;
    first_section->next = NULL;
    prev_section = &first_section->next;
    prev_key     = &first_section->key;

    while (fgets( buffer, PROFILE_MAX_LINE_LEN, file ))
    {
        line++;
        p = buffer + strlen(buffer) - 1;
        while ((p > buffer) && ((*p == '\n') || isspace(*p))) *p-- = '\0';
        p = buffer;
        while (*p && isspace(*p)) p++;
        if (*p == '[')  /* section start */
        {
            if (!(p2 = strrchr( p, ']' )))
            {
                fprintf( stderr, "PROFILE_Load: Invalid section header at line %d: '%s'\n",
                         line, p );
            }
            else
            {
                *p2 = '\0';
                p++;
                section = (PROFILESECTION *)xmalloc( sizeof(*section));
                section->name = xstrdup( p );
                section->key  = NULL;
                section->next = NULL;
                *prev_section = section;
                prev_section  = &section->next;
                prev_key      = &section->key;
                continue;
            }
        }
        if ((p2 = strchr( p, '=' )) != NULL)
        {
            char *p3 = p2 - 1;
            while ((p3 > p) && isspace(*p3)) *p3-- = '\0';
            *p2++ = '\0';
            while (*p2 && isspace(*p2)) p2++;
        }
        key = (PROFILEKEY *)xmalloc( sizeof(*key) );
        key->name  = xstrdup( p );
        key->value = p2 ? xstrdup( p2 ) : NULL;
        key->next  = NULL;
        *prev_key  = key;
        prev_key = &key->next;
    }
    if (debugging_profile)
    {
        fprintf( stddeb, "PROFILE_Load:\n" );
        PROFILE_Save( stddeb, first_section );
        fprintf( stddeb, "PROFILE_Load finished.\n" );
    }
    return first_section;
}


/***********************************************************************
 *           PROFILE_DeleteSection
 *
 * Delete a section from a profile tree.
 */
static BOOL32 PROFILE_DeleteSection( PROFILESECTION **section, LPCSTR name )
{
    while (*section)
    {
        if ((*section)->name && !lstrcmpi32A( (*section)->name, name ))
        {
            PROFILESECTION *to_del = *section;
            *section = to_del->next;
            to_del->next = NULL;
            PROFILE_Free( to_del );
            return TRUE;
        }
        section = &(*section)->next;
    }
    return FALSE;
}


/***********************************************************************
 *           PROFILE_DeleteKey
 *
 * Delete a key from a profile tree.
 */
static BOOL32 PROFILE_DeleteKey( PROFILESECTION **section,
                                 LPCSTR section_name, LPCSTR key_name )
{
    while (*section)
    {
        if ((*section)->name && !lstrcmpi32A( (*section)->name, section_name ))
        {
            PROFILEKEY **key = &(*section)->key;
            while (*key)
            {
                if (!lstrcmpi32A( (*key)->name, key_name ))
                {
                    PROFILEKEY *to_del = *key;
                    *key = to_del->next;
                    if (to_del->name) free( to_del->name );
                    if (to_del->value) free( to_del->value );
                    free( to_del );
                    return TRUE;
                }
                key = &(*key)->next;
            }
        }
        section = &(*section)->next;
    }
    return FALSE;
}


/***********************************************************************
 *           PROFILE_Find
 *
 * Find a key in a profile tree, optionally creating it.
 */
static PROFILEKEY *PROFILE_Find( PROFILESECTION **section,
                                 const char *section_name,
                                 const char *key_name, int create )
{
    while (*section)
    {
        if ((*section)->name && !lstrcmpi32A( (*section)->name, section_name ))
        {
            PROFILEKEY **key = &(*section)->key;
            while (*key)
            {
                if (!lstrcmpi32A( (*key)->name, key_name )) return *key;
                key = &(*key)->next;
            }
            if (!create) return NULL;
            *key = (PROFILEKEY *)xmalloc( sizeof(PROFILEKEY) );
            (*key)->name  = xstrdup( key_name );
            (*key)->value = NULL;
            (*key)->next  = NULL;
            return *key;
        }
        section = &(*section)->next;
    }
    if (!create) return NULL;
    *section = (PROFILESECTION *)xmalloc( sizeof(PROFILESECTION) );
    (*section)->name = xstrdup(section_name);
    (*section)->next = NULL;
    (*section)->key  = (PROFILEKEY *)xmalloc( sizeof(PROFILEKEY) );
    (*section)->key->name  = xstrdup( key_name );
    (*section)->key->value = NULL;
    (*section)->key->next  = NULL;
    return (*section)->key;
}


/***********************************************************************
 *           PROFILE_FlushFile
 *
 * Flush the current profile to disk if changed.
 */
static BOOL32 PROFILE_FlushFile(void)
{
    char *p, buffer[MAX_PATHNAME_LEN];
    const char *unix_name;
    FILE *file = NULL;

    if (!CurProfile.changed || !CurProfile.dos_name) return TRUE;
    if (!(unix_name = DOSFS_GetUnixFileName( CurProfile.dos_name, FALSE )) ||
        !(file = fopen( unix_name, "w" )))
    {
        /* Try to create it in $HOME/.wine */
        /* FIXME: this will need a more general solution */
        if ((p = getenv( "HOME" )) != NULL)
        {
            strcpy( buffer, p );
            strcat( buffer, "/.wine/" );
            p = buffer + strlen(buffer);
            strcpy( p, strrchr( CurProfile.dos_name, '\\' ) + 1 );
            AnsiLower( p );
            file = fopen( buffer, "w" );
            unix_name = buffer;
        }
    }
    
    if (!file)
    {
        fprintf( stderr, "Warning: could not save profile file %s\n",
                 CurProfile.dos_name );
        return FALSE;
    }

    dprintf_profile( stddeb, "Saving '%s' into '%s'\n",
                     CurProfile.dos_name, unix_name );
    PROFILE_Save( file, CurProfile.section );
    fclose( file );
    CurProfile.changed = FALSE;
    return TRUE;
}


/***********************************************************************
 *           PROFILE_Open
 *
 * Open a profile file, checking the cached file first.
 */
static BOOL32 PROFILE_Open( LPCSTR filename )
{
    char buffer[MAX_PATHNAME_LEN];
    const char *dos_name, *unix_name;
    char *newdos_name, *p;
    FILE *file = NULL;

    if (strchr( filename, '/' ) || strchr( filename, '\\' ) || 
        strchr( filename, ':' ))
    {
        if (!(dos_name = DOSFS_GetDosTrueName( filename, FALSE))) return FALSE;
    }
    else
    {
        GetWindowsDirectory32A( buffer, sizeof(buffer) );
        strcat( buffer, "\\" );
        strcat( buffer, filename );
        if (!(dos_name = DOSFS_GetDosTrueName( buffer, FALSE ))) return FALSE;
    }
    if (CurProfile.dos_name && !strcmp( dos_name, CurProfile.dos_name ))
    {
        dprintf_profile( stddeb, "PROFILE_Open(%s): already opened\n",
                         filename );
        return TRUE;
    }

    /* Flush the previous profile */

    newdos_name = xstrdup( dos_name );
    PROFILE_FlushFile();
    PROFILE_Free( CurProfile.section );
    if (CurProfile.dos_name) free( CurProfile.dos_name );
    CurProfile.section   = NULL;
    CurProfile.dos_name  = newdos_name;

    /* Try to open the profile file, first in $HOME/.wine */

    /* FIXME: this will need a more general solution */
    if ((p = getenv( "HOME" )) != NULL)
    {
        strcpy( buffer, p );
        strcat( buffer, "/.wine/" );
        p = buffer + strlen(buffer);
        strcpy( p, strrchr( newdos_name, '\\' ) + 1 );
        AnsiLower( p );
        if ((file = fopen( buffer, "r" )))
            dprintf_profile( stddeb, "Found it in %s\n", buffer );
    }

    if (!file && ((unix_name = DOSFS_GetUnixFileName( dos_name, TRUE ))))
    {
        if ((file = fopen( unix_name, "r" )))
            dprintf_profile( stddeb, "Found it in %s\n", unix_name );
    }

    if (file)
    {
        CurProfile.section = PROFILE_Load( file );
        fclose( file );
    }
    else
    {
        /* Does not exist yet, we will create it in PROFILE_FlushFile */
        fprintf( stderr, "Warning: profile file %s not found\n", newdos_name );
    }
    dprintf_profile( stddeb, "PROFILE_Open(%s): successful\n", filename );
    return TRUE;
}


/***********************************************************************
 *           PROFILE_GetSection
 *
 * Enumerate all the keys of a section.
 */
static INT32 PROFILE_GetSection( PROFILESECTION *section, LPCSTR section_name,
                                 LPSTR buffer, INT32 len, BOOL32 handle_env )
{
    PROFILEKEY *key;
    while (section)
    {
        if (section->name && !lstrcmpi32A( section->name, section_name ))
        {
            INT32 oldlen = len;
            for (key = section->key; key; key = key->next)
            {
                if (len <= 2) break;
                if (IS_ENTRY_COMMENT(key->name)) continue;  /* Skip comments */
                PROFILE_CopyEntry( buffer, key->name, len - 1, handle_env );
                len -= strlen(buffer) + 1;
                buffer += strlen(buffer) + 1;
            }
            *buffer = '\0';
            return oldlen - len + 1;
        }
        section = section->next;
    }
    buffer[0] = buffer[1] = '\0';
    return 2;
}


/***********************************************************************
 *           PROFILE_GetString
 *
 * Get a profile string.
 */
static INT32 PROFILE_GetString( LPCSTR section, LPCSTR key_name,
                                LPCSTR def_val, LPSTR buffer, INT32 len )
{
    PROFILEKEY *key = NULL;

    if (!def_val) def_val = "";
    if (key_name)
    {
        key = PROFILE_Find( &CurProfile.section, section, key_name, FALSE );
        PROFILE_CopyEntry( buffer, (key && key->value) ? key->value : def_val,
                           len, FALSE );
        dprintf_profile( stddeb, "PROFILE_GetString('%s','%s','%s'): returning '%s'\n",
                         section, key_name, def_val, buffer );
        return strlen( buffer );
    }
    return PROFILE_GetSection(CurProfile.section, section, buffer, len, FALSE);
}


/***********************************************************************
 *           PROFILE_SetString
 *
 * Set a profile string.
 */
static BOOL32 PROFILE_SetString( LPCSTR section_name, LPCSTR key_name,
                                 LPCSTR value )
{
    BOOL32 ret;

    if (!key_name)  /* Delete a whole section */
    {
        dprintf_profile(stddeb, "PROFILE_DeleteSection('%s')\n", section_name);
        ret = PROFILE_DeleteSection( &CurProfile.section, section_name );
        CurProfile.changed |= ret;
        return ret;
    }
    else if (!value)  /* Delete a key */
    {
        dprintf_profile( stddeb, "PROFILE_DeleteKey('%s','%s')\n",
                         section_name, key_name );
        ret = PROFILE_DeleteKey( &CurProfile.section, section_name, key_name );
        CurProfile.changed |= ret;
        return ret;
    }
    else  /* Set the key value */
    {
        PROFILEKEY *key = PROFILE_Find( &CurProfile.section, section_name,
                                        key_name, TRUE );
        dprintf_profile( stddeb, "PROFILE_SetString('%s','%s','%s'): ",
                         section_name, key_name, value );
        if (key->value)
        {
            if (!strcmp( key->value, value ))
            {
                dprintf_profile( stddeb, "no change needed\n" );
                return TRUE;  /* No change needed */
            }
            dprintf_profile( stddeb, "replacing '%s'\n", key->value );
            free( key->value );
        }
        else dprintf_profile( stddeb, "creating key\n" );
        key->value = xstrdup( value );
        CurProfile.changed = TRUE;
        return TRUE;
    }
}


/***********************************************************************
 *           PROFILE_GetWineIniString
 *
 * Get a config string from the wine.ini file.
 */
int PROFILE_GetWineIniString( const char *section, const char *key_name,
                              const char *def, char *buffer, int len )
{
    if (key_name)
    {
        PROFILEKEY *key = PROFILE_Find(&WineProfile, section, key_name, FALSE);
        PROFILE_CopyEntry( buffer, (key && key->value) ? key->value : def,
                           len, TRUE );
        dprintf_profile( stddeb, "PROFILE_GetWineIniString('%s','%s','%s'): returning '%s'\n",
                         section, key_name, def, buffer );
        return strlen( buffer );
    }
    return PROFILE_GetSection( WineProfile, section, buffer, len, TRUE );
}


/***********************************************************************
 *           PROFILE_GetWineIniInt
 *
 * Get a config integer from the wine.ini file.
 */
int PROFILE_GetWineIniInt( const char *section, const char *key_name, int def )
{
    char buffer[20];
    char *p;
    long result;

    PROFILEKEY *key = PROFILE_Find( &WineProfile, section, key_name, FALSE );
    if (!key || !key->value) return def;
    PROFILE_CopyEntry( buffer, key->value, sizeof(buffer), TRUE );
    result = strtol( buffer, &p, 0 );
    if (p == buffer) return 0;  /* No digits at all */
    return (int)result;
}


/***********************************************************************
 *           PROFILE_LoadWineIni
 *
 * Load the wine.ini file.
 */
int PROFILE_LoadWineIni(void)
{
    char buffer[MAX_PATHNAME_LEN];
    const char *p;
    FILE *f;

    if ((p = getenv( "HOME" )) != NULL)
    {
        lstrcpyn32A(buffer, p, MAX_PATHNAME_LEN - sizeof(PROFILE_WineIniName));
        strcat( buffer, PROFILE_WineIniName );
        if ((f = fopen( buffer, "r" )) != NULL)
        {
            WineProfile = PROFILE_Load( f );
            fclose( f );
            return 1;
        }
    }
    else fprintf( stderr, "Warning: could not get $HOME value for config file.\n" );

    /* Try global file */

    if ((f = fopen( WINE_INI_GLOBAL, "r" )) != NULL)
    {
        WineProfile = PROFILE_Load( f );
        fclose( f );
        return 1;
    }
    fprintf( stderr, "Can't open configuration file %s or $HOME%s\n",
             WINE_INI_GLOBAL, PROFILE_WineIniName );
    return 0;
}


/********************* API functions **********************************/

/***********************************************************************
 *           GetProfileInt16   (KERNEL.57)
 */
UINT16 GetProfileInt16( LPCSTR section, LPCSTR entry, INT16 def_val )
{
    return GetPrivateProfileInt16( section, entry, def_val, "win.ini" );
}


/***********************************************************************
 *           GetProfileInt32A   (KERNEL32.264)
 */
UINT32 GetProfileInt32A( LPCSTR section, LPCSTR entry, INT32 def_val )
{
    return GetPrivateProfileInt32A( section, entry, def_val, "win.ini" );
}

/***********************************************************************
 *           GetProfileInt32W   (KERNEL32.264)
 */
UINT32 GetProfileInt32W( LPCWSTR section, LPCWSTR entry, INT32 def_val )
{
    if (!wininiW) wininiW = STRING32_DupAnsiToUni("win.ini");
    return GetPrivateProfileInt32W( section, entry, def_val, wininiW );
}

/***********************************************************************
 *           GetProfileString16   (KERNEL.58)
 */
INT16 GetProfileString16( LPCSTR section, LPCSTR entry, LPCSTR def_val,
                          LPSTR buffer, INT16 len )
{
    return GetPrivateProfileString16( section, entry, def_val,
                                      buffer, len, "win.ini" );
}

/***********************************************************************
 *           GetProfileString32A   (KERNEL32.268)
 */
INT32 GetProfileString32A( LPCSTR section, LPCSTR entry, LPCSTR def_val,
                           LPSTR buffer, INT32 len )
{
    return GetPrivateProfileString32A( section, entry, def_val,
                                       buffer, len, "win.ini" );
}

/***********************************************************************
 *           GetProfileString32W   (KERNEL32.269)
 */
INT32 GetProfileString32W( LPCWSTR section,LPCWSTR entry,LPCWSTR def_val,
                           LPWSTR buffer, INT32 len )
{
    if (!wininiW) wininiW = STRING32_DupAnsiToUni("win.ini");
    return GetPrivateProfileString32W( section, entry, def_val,
                                       buffer, len, wininiW );
}


/***********************************************************************
 *           WriteProfileString16   (KERNEL.59)
 */
BOOL16 WriteProfileString16( LPCSTR section, LPCSTR entry, LPCSTR string )
{
    return WritePrivateProfileString16( section, entry, string, "win.ini" );
}

/***********************************************************************
 *           WriteProfileString32A   (KERNEL32.587)
 */
BOOL32 WriteProfileString32A( LPCSTR section, LPCSTR entry, LPCSTR string )
{
    return WritePrivateProfileString32A( section, entry, string, "win.ini" );
}

/***********************************************************************
 *           WriteProfileString32W   (KERNEL32.588)
 */
BOOL32 WriteProfileString32W( LPCWSTR section, LPCWSTR entry, LPCWSTR string )
{
    if (!wininiW) wininiW = STRING32_DupAnsiToUni("win.ini");
    return WritePrivateProfileString32W( section, entry, string, wininiW );
}


/***********************************************************************
 *           GetPrivateProfileInt16   (KERNEL.127)
 */
UINT16 GetPrivateProfileInt16( LPCSTR section, LPCSTR entry, INT16 def_val,
                               LPCSTR filename )
{
    long result=(long)GetPrivateProfileInt32A(section,entry,def_val,filename);

    if (result > 65535) return 65535;
    if (result >= 0) return (UINT16)result;
    if (result < -32768) return -32768;
    return (UINT16)(INT16)result;
}

/***********************************************************************
 *           GetPrivateProfileInt32A   (KERNEL32.251)
 */
UINT32 GetPrivateProfileInt32A( LPCSTR section, LPCSTR entry, INT32 def_val,
                                LPCSTR filename )
{
    char buffer[20];
    char *p;
    long result;

    GetPrivateProfileString32A( section, entry, "",
                                buffer, sizeof(buffer), filename );
    if (!buffer[0]) return (UINT32)def_val;
    result = strtol( buffer, &p, 0 );
    if (p == buffer) return 0;  /* No digits at all */
    return (UINT32)result;
}

/***********************************************************************
 *           GetPrivateProfileInt32W   (KERNEL32.252)
 */
UINT32 GetPrivateProfileInt32W( LPCWSTR section, LPCWSTR entry, INT32 def_val,
                                LPCWSTR filename )
{
    LPSTR sectionA=section?STRING32_DupUniToAnsi(section):NULL;
    LPSTR entryA=entry?STRING32_DupUniToAnsi(entry):NULL;
    LPSTR filenameA=filename?STRING32_DupUniToAnsi(filename):NULL;
    UINT32 res;

    res=GetPrivateProfileInt32A(sectionA,entryA,def_val,filenameA);
    if (sectionA) free(sectionA);
    if (filenameA) free(filenameA);
    if (entryA) free(entryA);
    return res;
}

/***********************************************************************
 *           GetPrivateProfileString16   (KERNEL.128)
 */
INT16 GetPrivateProfileString16( LPCSTR section, LPCSTR entry, LPCSTR def_val,
                                 LPSTR buffer, INT16 len, LPCSTR filename )
{
    return GetPrivateProfileString32A(section,entry,def_val,buffer,len,filename);
}

/***********************************************************************
 *           GetPrivateProfileString32A   (KERNEL32.255)
 */
INT32 GetPrivateProfileString32A( LPCSTR section, LPCSTR entry, LPCSTR def_val,
                                  LPSTR buffer, INT32 len, LPCSTR filename )
{
    if (PROFILE_Open( filename ))
        return PROFILE_GetString( section, entry, def_val, buffer, len );
    lstrcpyn32A( buffer, def_val, len );
    return strlen( buffer );
}

/***********************************************************************
 *           GetPrivateProfileString32W   (KERNEL32.256)
 */
INT32 GetPrivateProfileString32W( LPCWSTR section,LPCWSTR entry,LPCWSTR def_val,
                                  LPWSTR buffer,INT32 len,LPCWSTR filename )
{
    LPSTR	sectionA = section?STRING32_DupUniToAnsi(section):NULL;
    LPSTR	entryA = entry?STRING32_DupUniToAnsi(entry):NULL;
    LPSTR	filenameA = filename?STRING32_DupUniToAnsi(filename):NULL;
    LPSTR	def_valA = def_val?STRING32_DupUniToAnsi(def_val):NULL;
    LPSTR	bufferA = xmalloc(len);
    INT32	ret;

    ret=GetPrivateProfileString32A(sectionA,entryA,def_valA,bufferA,len,filenameA);
    if (sectionA) free(sectionA);
    if (entryA) free(entryA);
    if (filenameA) free(filenameA);
    if (def_valA) free(def_valA);

    lstrcpynAtoW( buffer, bufferA, len );
    free(bufferA);
    return ret;
}



/***********************************************************************
 *           WritePrivateProfileString16   (KERNEL.129)
 */
BOOL16 WritePrivateProfileString16(LPCSTR section,LPCSTR entry,LPCSTR string,
                                   LPCSTR filename)
{
    return WritePrivateProfileString32A(section,entry,string,filename);
}

/***********************************************************************
 *           WritePrivateProfileString32A   (KERNEL32.582)
 */
BOOL32 WritePrivateProfileString32A(LPCSTR section,LPCSTR entry,LPCSTR string,
                                    LPCSTR filename )
{
    if (!PROFILE_Open( filename )) return FALSE;
    if (!section) return PROFILE_FlushFile();
    return PROFILE_SetString( section, entry, string );
}

/***********************************************************************
 *           WritePrivateProfileString32W   (KERNEL32.583)
 */
BOOL32 WritePrivateProfileString32W(LPCWSTR section,LPCWSTR entry,LPCWSTR string,
                                    LPCWSTR filename )
{
    LPSTR sectionA = section?STRING32_DupUniToAnsi(section):NULL;
    LPSTR entryA = entry?STRING32_DupUniToAnsi(entry):NULL;
    LPSTR stringA = string?STRING32_DupUniToAnsi(string):NULL;
    LPSTR filenameA = filename?STRING32_DupUniToAnsi(filename):NULL;
    BOOL32 res;

    res = WritePrivateProfileString32A(sectionA,entryA,stringA,filenameA);

    if (sectionA) free(sectionA);
    if (entryA) free(entryA);
    if (stringA) free(stringA);
    if (filenameA) free(filenameA);
    return res;
}


/***********************************************************************
 *           WriteOutProfiles   (KERNEL.315)
 */
void WriteOutProfiles(void)
{
    PROFILE_FlushFile();
}
