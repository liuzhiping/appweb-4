/*
    sdb.c -- SQLite Database (SDB)

    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */

/********************************** Includes **********************************/

#include    "appweb.h"

#if BIT_PACK_ESP && BIT_PACK_SDB
 #include    "edi.h"
 #include    "sqlite3.h"

#ifndef BIT_MAX_SQLITE_MEM
    #define BIT_MAX_SQLITE_MEM      (2*1024*1024)   /**< Maximum buffering for Sqlite */
#endif
#ifndef BIT_MAX_SQLITE_DURATION
    #define BIT_MAX_SQLITE_DURATION 30000           /**< Database busy timeout */
#endif

/************************************* Local **********************************/

typedef struct Sdb {
    Edi             edi;            /**< EDI database interface structure */
    sqlite3         *db;            /**< SQLite database handle */
    MprHash         *validations;   /**< Validations */
    MprHash         *schemas;       /**< Table schemas */
} Sdb;

static int sqliteInitialized;
static void initSqlite();

#if KEEP
static char *DataTypeToSqlType[] = {
    "binary":       "blob",
    "boolean":      "tinyint",
    "date":         "date",
    "datetime":     "datetime",
    "decimal":      "decimal",
    "float":        "float",
    "integer":      "int",
    "number":       "decimal",
    "string":       "varchar",
    "text":         "text",
    "time":         "time",
    "timestamp":    "datetime",
    0, 0, 
};
#endif

static char *dataTypeToSqlType[] = {
                            0,
    /* EDI_TYPE_BINARY */   "BLOB",
    /* EDI_TYPE_BOOL */     "TINYINT",      //  "INTEGER",
    /* EDI_TYPE_DATE */     "DATE",         //  "TEXT",
    /* EDI_TYPE_FLOAT */    "FLOAT",        //  "REAL",
    /* EDI_TYPE_INT */      "INTEGER",
    /* EDI_TYPE_STRING */   "STRING",       //  "TEXT",
    /* EDI_TYPE_TEXT */     "TEXT",
                            0,
};

/************************************ Forwards ********************************/

static EdiRec *createBareRec(Edi *edi, cchar *tableName, int nfields);
static EdiField makeRecField(cchar *value, cchar *name, int type);
static void manageSdb(Sdb *sdb, int flags);
static int mapSqliteTypeToEdiType(int type);
static cchar *mapToSqlType(int type);
static int mapToEdiType(cchar *type);
static EdiGrid *query(Edi *edi, cchar *cmd, ...);
static EdiGrid *queryv(Edi *edi, cchar *cmd, int argc, cchar **argv, va_list args);
static int sdbAddColumn(Edi *edi, cchar *tableName, cchar *columnName, int type, int flags);
static int sdbAddIndex(Edi *edi, cchar *tableName, cchar *columnName, cchar *indexName);
static int sdbAddTable(Edi *edi, cchar *tableName);
static int sdbAddValidation(Edi *edi, cchar *tableName, cchar *fieldName, EdiValidation *vp);
static int sdbChangeColumn(Edi *edi, cchar *tableName, cchar *columnName, int type, int flags);
static void sdbClose(Edi *edi);
static EdiRec *sdbCreateRec(Edi *edi, cchar *tableName);
static int sdbDelete(cchar *path);
static void sdbError(Edi *edi, cchar *fmt, ...);
static int sdbRemoveRec(Edi *edi, cchar *tableName, cchar *key);
static MprList *sdbGetColumns(Edi *edi, cchar *tableName);
static int sdbGetColumnSchema(Edi *edi, cchar *tableName, cchar *columnName, int *type, int *flags, int *cid);
static MprList *sdbGetTables(Edi *edi);
static int sdbGetTableSchema(Edi *edi, cchar *tableName, int *numRows, int *numCols);
static int sdbLookupField(Edi *edi, cchar *tableName, cchar *fieldName);
static Edi *sdbOpen(cchar *path, int flags);
PUBLIC EdiGrid *sdbQuery(Edi *edi, cchar *cmd, int argc, cchar **argv, va_list vargs);
static EdiField sdbReadField(Edi *edi, cchar *tableName, cchar *key, cchar *fieldName);
static EdiRec *sdbReadRec(Edi *edi, cchar *tableName, cchar *key);
static EdiGrid *sdbReadWhere(Edi *edi, cchar *tableName, cchar *columnName, cchar *operation, cchar *value);
static int sdbRemoveColumn(Edi *edi, cchar *tableName, cchar *columnName);
static int sdbRemoveIndex(Edi *edi, cchar *tableName, cchar *indexName);
static int sdbRemoveTable(Edi *edi, cchar *tableName);
static int sdbRenameTable(Edi *edi, cchar *tableName, cchar *newTableName);
static int sdbRenameColumn(Edi *edi, cchar *tableName, cchar *columnName, cchar *newColumnName);
static int sdbSave(Edi *edi);
static void sdbTrace(Edi *edi, int level, cchar *fmt, ...);
static int sdbUpdateField(Edi *edi, cchar *tableName, cchar *key, cchar *fieldName, cchar *value);
static int sdbUpdateRec(Edi *edi, EdiRec *rec);
static bool sdbValidateRec(Edi *edi, EdiRec *rec);
static bool validateField(Sdb *sdb, EdiRec *rec, cchar *tableName, cchar *columnName, cchar *value);
static bool validName(cchar *str);

static EdiProvider SdbProvider = {
    "sdb",
    sdbAddColumn, sdbAddIndex, sdbAddTable, sdbAddValidation, sdbChangeColumn, sdbClose, sdbCreateRec, sdbDelete, 
    sdbGetColumns, sdbGetColumnSchema, sdbGetTables, sdbGetTableSchema, NULL, sdbLookupField, sdbOpen, sdbQuery, 
    sdbReadField, sdbReadRec, sdbReadWhere, sdbRemoveColumn, sdbRemoveIndex, sdbRemoveRec, sdbRemoveTable, 
    sdbRenameTable, sdbRenameColumn, sdbSave, sdbUpdateField, sdbUpdateRec, sdbValidateRec,
};

/************************************* Code ***********************************/

PUBLIC void sdbInit()
{
    ediAddProvider(&SdbProvider);
}


static Sdb *sdbCreate(cchar *path, int flags)
{
    Sdb      *sdb;

    assert(path && *path);

    initSqlite();
    if ((sdb = mprAllocObj(Sdb, manageSdb)) == 0) {
        return 0;
    }
    sdb->edi.flags = flags;
    sdb->edi.provider = &SdbProvider;
    sdb->edi.path = sclone(path);
    sdb->edi.schemaCache = mprCreateHash(0, 0);
    sdb->schemas = mprCreateHash(0, MPR_HASH_STABLE);
    sdb->validations = mprCreateHash(0, MPR_HASH_STABLE);
    return sdb;
}


static void manageSdb(Sdb *sdb, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(sdb->edi.path);
        mprMark(sdb->edi.schemaCache);
        mprMark(sdb->edi.errMsg);
        mprMark(sdb->schemas);
        mprMark(sdb->validations);

    } else if (flags & MPR_MANAGE_FREE) {
        sdbClose((Edi*) sdb);
    }
}


static void sdbClose(Edi *edi)
{
    Sdb     *sdb;

    assert(edi);

    sdb = (Sdb*) edi;
    if (sdb->db) {
        sqlite3_close(sdb->db);
        sdb->db = 0;
    }
}


static void removeSchema(Edi *edi, cchar *tableName)
{
    mprRemoveKey(((Sdb*) edi)->schemas, tableName);
}


static EdiRec *getSchema(Edi *edi, cchar *tableName)
{
    Sdb         *sdb;
    EdiRec      *rec, *schema;
    EdiField    *fp;
    EdiGrid     *grid;
    int         r;

    sdb = (Sdb*) edi;
    if (!validName(tableName)) {
        return 0;
    }
    if ((schema = mprLookupKey(sdb->schemas, tableName)) != 0) {
        return schema;
    }
    /*
        Each result row represents an EDI column: CID, Name, Type, NotNull, Dflt_value, PK
     */
    if ((grid = query(edi, sfmt("PRAGMA table_info(%s);", tableName), NULL)) == 0) {
        return 0;
    }
    schema = createBareRec(edi, tableName, grid->nrecords);
    for (r = 0; r < grid->nrecords; r++) {
        rec = grid->records[r];
        fp = &schema->fields[r];
        fp->name = rec->fields[1].value;
        fp->type = mapToEdiType(rec->fields[2].value);
        if (rec->fields[5].value && rec->fields[5].value[0] == '1') {
            fp->flags = EDI_KEY;
        }
    }
    mprAddKey(sdb->schemas, tableName, schema);
    return schema;
}


static EdiRec *sdbCreateRec(Edi *edi, cchar *tableName)
{
    EdiRec  *rec, *schema;
    int     i;

    schema = getSchema(edi, tableName);
    if ((rec = mprAllocMem(sizeof(EdiRec) + sizeof(EdiField) * schema->nfields, MPR_ALLOC_MANAGER | MPR_ALLOC_ZERO)) == 0) {
        return 0;
    }
    mprSetManager(rec, (MprManager) ediManageEdiRec);
    rec->edi = edi;
    rec->tableName = sclone(tableName);
    rec->nfields = schema->nfields;
    for (i = 0; i < schema->nfields; i++) {
        rec->fields[i].name = schema->fields[i].name;
        rec->fields[i].type = schema->fields[i].type;
        rec->fields[i].flags = schema->fields[i].flags;
    }
    return rec;
}


static int sdbDelete(cchar *path)
{
    assert(path && *path);
    return mprDeletePath(path);
}


static Edi *sdbOpen(cchar *path, int flags)
{
    Sdb     *sdb;

    assert(path && *path);
    if ((sdb = sdbCreate(path, flags)) == 0) {
        return 0;
    }
    if (mprPathExists(path, R_OK) || (flags & EDI_CREATE)) {
        if (sqlite3_open(path, &sdb->db) != SQLITE_OK) {
            mprError("Cannot open database %s", path);
            return 0;
        }
        sqlite3_soft_heap_limit(BIT_MAX_SQLITE_MEM);
        sqlite3_busy_timeout(sdb->db, BIT_MAX_SQLITE_DURATION);
    } else {
        return 0;
    }
    return (Edi*) sdb;
}


static int sdbAddColumn(Edi *edi, cchar *tableName, cchar *columnName, int type, int flags)
{
    assert(edi);
    assert(tableName && *tableName);
    assert(columnName && *columnName);
    assert(type > 0);

    if (!validName(tableName) || !validName(columnName)) {
        return MPR_ERR_BAD_ARGS;
    }
    if (sdbLookupField(edi, tableName, columnName) >= 0) {
        /* Already exists */
        return 0;
    }
    removeSchema(edi, tableName);
    /*
        The field types are used for the SQLite column affinity settings
     */
    if (query(edi, sfmt("ALTER TABLE %s ADD %s %s", tableName, columnName, mapToSqlType(type)), NULL) == 0) {
        return MPR_ERR_CANT_CREATE;
    }
    return 0;
}


static int sdbAddIndex(Edi *edi, cchar *tableName, cchar *columnName, cchar *indexName)
{
    assert(edi);
    assert(tableName && *tableName);
    assert(columnName && *columnName);
    assert(indexName && *indexName);

    if (!validName(tableName) || !validName(columnName) || !validName(indexName)) {
        return MPR_ERR_BAD_ARGS;
    }
    return query(edi, sfmt("CREATE INDEX %s ON %s (%s);", indexName, tableName, columnName), NULL) != 0;
}


static int sdbAddTable(Edi *edi, cchar *tableName)
{
    assert(edi);
    assert(tableName && *tableName);

    if (!validName(tableName)) {
        return MPR_ERR_BAD_ARGS;
    }
    if (query(edi, sfmt("DROP TABLE IF EXISTS %s;", tableName), NULL) == 0) {
        return MPR_ERR_CANT_DELETE;
    }
    /*
        SQLite cannot add a primary key after the table is created
     */
    removeSchema(edi, tableName);
    return query(edi, sfmt("CREATE TABLE %s (id INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL);", tableName), NULL) != 0;

}


static int sdbAddValidation(Edi *edi, cchar *tableName, cchar *columnName, EdiValidation *vp)
{
    Sdb             *sdb;
    MprList         *validations;
    EdiValidation   *prior;
    cchar           *vkey;
    int             next;

    assert(edi);
    assert(tableName && *tableName);
    assert(columnName && *columnName);
    assert(vp);

    sdb = (Sdb*) edi;
    vkey = sfmt("%s.%s", tableName, columnName);
    if ((validations = mprLookupKey(sdb->validations, vkey)) == 0) {
        validations = mprCreateList(0, MPR_LIST_STABLE);
        mprAddKey(sdb->validations, vkey, validations);
    }
    for (ITERATE_ITEMS(validations, prior, next)) {
        if (prior->vfn == vp->vfn) {
            break;
        }
    }
    if (!prior) {
        mprAddItem(validations, vp);
    }
    return 0;
}


static int sdbChangeColumn(Edi *edi, cchar *tableName, cchar *columnName, int type, int flags)
{
    mprError("SDB does not support changing columns");
    return MPR_ERR_BAD_STATE;
}


static MprList *sdbGetColumns(Edi *edi, cchar *tableName)
{
    MprList     *result;
    EdiRec      *schema;
    int         c;

    assert(edi);
    assert(tableName && *tableName);
    
    if ((schema = getSchema(edi, tableName)) == 0) {
        return 0;
    }
    if ((result = mprCreateList(0, MPR_LIST_STABLE)) == 0) {
        return 0;
    }
    for (c = 0; c < schema->nfields; c++) {
        mprAddItem(result, schema->fields[c].name);
    }
    return result;
}



static int sdbGetColumnSchema(Edi *edi, cchar *tableName, cchar *columnName, int *type, int *flags, int *cid)
{
    EdiRec      *schema;
    EdiField    *fp;
    int         c;

    assert(edi);
    assert(tableName && *tableName);
    assert(columnName && *columnName);

    schema = getSchema(edi, tableName);
    for (c = 0; c < schema->nfields; c++) {
        fp = &schema->fields[c];
        if (smatch(columnName, fp->name)) {
            if (type) {
                *type = fp->type;
            }
            if (flags) {
                *flags = fp->flags;
            }
            if (cid) {
                *cid = c;
            }
        }
    }
    return 0;
}


static MprList *sdbGetTables(Edi *edi)
{
    EdiGrid     *grid;
    EdiRec      *rec;
    MprList     *result;
    int         r;

    assert(edi);

    if ((grid = query(edi, "SELECT name from sqlite_master WHERE type = 'table' order by NAME;", NULL)) == 0) {
        return 0;
    }
    if ((result = mprCreateList(0, MPR_LIST_STABLE)) == 0) {
        return 0;
    }
    for (r = 0; r < grid->nrecords; r++) {
        rec = grid->records[r];
        if (sstarts(rec->tableName, "sqlite_")) {
            continue;
        }
        mprAddItem(result, rec->tableName);
    }
    return result;
}


static int sdbGetTableSchema(Edi *edi, cchar *tableName, int *numRows, int *numCols)
{
    EdiGrid     *grid;
    EdiRec      *schema;

    assert(edi);
    assert(tableName && *tableName);

    if (!validName(tableName)) {
        return MPR_ERR_BAD_ARGS;
    }
    if (numRows) {
        if ((grid = query(edi, sfmt("SELECT COUNT(*) FROM %s;", tableName), NULL)) == 0) { 
            return MPR_ERR_BAD_STATE;
        }
        *numRows = grid->nrecords;
    }
    if (numCols) {
        if ((schema = getSchema(edi, tableName)) == 0) {
            return MPR_ERR_CANT_FIND;
        }
        *numCols = schema->nfields;
    }
    return 0;
}


static int sdbLookupField(Edi *edi, cchar *tableName, cchar *fieldName)
{
    EdiRec      *schema;
    int         c;

    assert(edi);
    assert(tableName && *tableName);
    assert(fieldName && *fieldName);
    
    if ((schema = getSchema(edi, tableName)) == 0) {
        return 0;
    }
    for (c = 0; c < schema->nfields; c++) {
        /* Name is second field */
        if (smatch(fieldName, schema->fields[c].name)) {
            return c;
        }
    }
    return MPR_ERR_CANT_FIND;
}


PUBLIC EdiGrid *sdbQuery(Edi *edi, cchar *cmd, int argc, cchar **argv, va_list vargs)
{
    return queryv(edi, cmd, argc, argv, vargs);
}


static EdiField sdbReadField(Edi *edi, cchar *tableName, cchar *key, cchar *fieldName)
{
    EdiField    err;
    EdiGrid     *grid;

    if (!validName(tableName) || !validName(fieldName)) {
        err.valid = 0;
        return err;
    }
    if ((grid = query(edi, sfmt("SELECT %s FROM %s WHERE 'id' = ?;", fieldName, tableName), key, NULL)) == 0) {
        err.valid = 0;
        return err;
    }
    return grid->records[0]->fields[0];
}


static EdiRec *sdbReadRec(Edi *edi, cchar *tableName, cchar *key)
{
    EdiGrid     *grid;

    if (!validName(tableName)) {
        return 0;
    }
    if ((grid = query(edi, sfmt("SELECT * FROM %s WHERE id = ?;", tableName), key, NULL)) == 0) {
        return 0;
    }
    return grid->records[0];
}


static EdiGrid *setTableName(EdiGrid *grid, cchar *tableName)
{
    if (grid && !grid->tableName) {
        grid->tableName = sclone(tableName);
    }
    return grid;
}


static EdiGrid *sdbReadWhere(Edi *edi, cchar *tableName, cchar *columnName, cchar *operation, cchar *value)
{
    EdiGrid     *grid;
    
    assert(tableName && *tableName);

    if (!validName(tableName)) {
        return 0;
    }
    if (columnName) {
        if (!validName(columnName)) {
            return 0;
        }
        assert(columnName && *columnName);
        assert(operation && *operation);
        assert(value);
        grid = query(edi, sfmt("SELECT * FROM %s WHERE %s %s ?;", tableName, columnName, operation), value, NULL);
    } else {
        grid = query(edi, sfmt("SELECT * FROM %s;", tableName), NULL);
    }
    return setTableName(grid, tableName);
}


static int sdbRemoveColumn(Edi *edi, cchar *tableName, cchar *columnName)
{
    mprError("SDB does not support removing columns");
    return MPR_ERR_BAD_STATE;
}


static int sdbRemoveIndex(Edi *edi, cchar *tableName, cchar *indexName)
{
    if (!validName(tableName) || !validName(indexName)) {
        return 0;
    }
    return query(edi, sfmt("DROP INDEX %s;", indexName), NULL) != 0;
}


static int sdbRemoveRec(Edi *edi, cchar *tableName, cchar *key)
{
    assert(edi);
    assert(tableName && *tableName);
    assert(key && *key);

    if (!validName(tableName)) {
        return 0;
    }
    return query(edi, sfmt("DELETE FROM %s WHERE id = ?;", tableName), key, NULL) != 0;
}


static int sdbRemoveTable(Edi *edi, cchar *tableName)
{
    if (!validName(tableName)) {
        return 0;
    }
    return query(edi, sfmt("DROP TABLE IF EXISTS %s;", tableName), NULL) != 0;
}


static int sdbRenameTable(Edi *edi, cchar *tableName, cchar *newTableName)
{
    if (!validName(tableName) || !validName(newTableName)) {
        return 0;
    }
    removeSchema(edi, tableName);
    removeSchema(edi, newTableName);
    return query(edi, sfmt("ALTER TABLE %s RENAME TO %s;", tableName, newTableName), NULL) != 0;
}


static int sdbRenameColumn(Edi *edi, cchar *tableName, cchar *columnName, cchar *newColumnName)
{
    mprError("SQLite does not support renaming columns");
    return MPR_ERR_BAD_STATE;
}


static int sdbSave(Edi *edi)
{
    return 0;
}


static bool sdbValidateRec(Edi *edi, EdiRec *rec)
{
    Sdb         *sdb;
    EdiField    *fp;
    bool        pass;
    int         c;

    if (!rec) {
        return 0;
    }
    sdb = (Sdb*) edi;
    pass = 1;
    for (c = 0; c < rec->nfields; c++) {
        fp = &rec->fields[c];
        if (!validateField(sdb, rec, rec->tableName, fp->name, fp->value)) {
            pass = 0;
            /* Keep going */
        }
    }
    return pass;
}


/*
    Map values before storing in the database
    While not required, it is prefereable to normalize the storage of some values.
    For example: dates are stored as numbers
 */
static cchar *mapSdbValue(cchar *value, int type)
{
    MprTime     time;

    if (value == 0) {
        return value;
    }
    switch (type) {
    case EDI_TYPE_DATE:
        if (!snumber(value)) {
            mprParseTime(&time, value, MPR_UTC_TIMEZONE, 0);
            value = itos(time);
        }
        break;

    case EDI_TYPE_BINARY:
    case EDI_TYPE_BOOL:
    case EDI_TYPE_FLOAT:
    case EDI_TYPE_INT:
    case EDI_TYPE_STRING:
    case EDI_TYPE_TEXT:
    default:
        break;
    }
    return value;
}


static int sdbUpdateField(Edi *edi, cchar *tableName, cchar *key, cchar *fieldName, cchar *value)
{
    int     type;

    if (!validName(tableName) || !validName(fieldName)) {
        return 0;
    }
    sdbGetColumnSchema(edi, tableName, fieldName, &type, 0, 0);
    value = mapSdbValue(value, type);
    return query(edi, sfmt("UPDATE %s SET %s TO ? WHERE 'id' = ?;", tableName, fieldName), value, key, NULL) != 0;
}


/*
    Use parameterized queries to reduce the risk of SQL injection
 */
static int sdbUpdateRec(Edi *edi, EdiRec *rec)
{
    MprBuf      *buf;
    EdiField    *fp;
    cchar       **argv;
    int         argc, f;

    if (!sdbValidateRec(edi, rec)) {
        return MPR_ERR_CANT_WRITE;
    }
    if ((argv = mprAlloc(((rec->nfields * 2) + 2) * sizeof(cchar*))) == 0) {
        return MPR_ERR_MEMORY;
    }
    argc = 0;

    buf = mprCreateBuf(0, 0);
    if (rec->id) {
        mprPutToBuf(buf, "UPDATE %s SET ", rec->tableName);
        for (f = 0; f < rec->nfields; f++) {
            fp = &rec->fields[f];
            mprPutToBuf(buf, "%s = ?, ", fp->name);
            argv[argc++] = mapSdbValue(fp->value, fp->type);
        }
        mprAdjustBufEnd(buf, -2);
        mprPutStringToBuf(buf, " WHERE id = ?;");
        argv[argc++] = rec->id;

    } else {
        mprPutToBuf(buf, "INSERT INTO %s (", rec->tableName);
        for (f = 1; f < rec->nfields; f++) {
            fp = &rec->fields[f];
            mprPutToBuf(buf, "%s,", fp->name);
        }
        mprAdjustBufEnd(buf, -1);
        mprPutStringToBuf(buf, ") VALUES (");
        for (f = 1; f < rec->nfields; f++) {
            mprPutStringToBuf(buf, "?,");
            fp = &rec->fields[f];
            argv[argc++] = mapSdbValue(fp->value, fp->type);
        }
        mprAdjustBufEnd(buf, -1);
        mprPutCharToBuf(buf, ')');
    }
    argv[argc] = NULL;

    if (queryv(edi, mprGetBufStart(buf), argc, argv, NULL) == 0) {
        return MPR_ERR_CANT_WRITE;
    }
    return 0;
}


PUBLIC cchar *sdbGetLastError(Edi *edi)
{
    return ((Sdb*) edi)->edi.errMsg;
}


/*********************************** Support *******************************/

static EdiGrid *query(Edi *edi, cchar *cmd, ...)
{
    EdiGrid     *grid;
    va_list     args;

    va_start(args, cmd);
    grid = queryv(edi, cmd, 0, NULL, args);
    va_end(args);
    return grid;
}


/*
    This function supports parameterized queries. Provide parameters for query via either argc+argv or vargs.
    Strongly recommended to use parameterized queries to lessen SQL injection risk.
 */
static EdiGrid *queryv(Edi *edi, cchar *cmd, int argc, cchar **argv, va_list vargs)
{
    Sdb             *sdb;
    sqlite3         *db;
    sqlite3_stmt    *stmt;
    EdiGrid         *grid;
    EdiRec          *rec, *schema;
    MprList         *result;
    char            *tableName;
    cchar           *tail, *colName, *value, *defaultTableName, *arg;
    ssize           len;
    int             r, nrows, i, ncol, rc, retries, index, type;

    assert(edi);
    assert(cmd && *cmd);

    sdb = (Sdb*) edi;
    retries = 0;
    sdb->edi.errMsg = 0;

    if ((db = sdb->db) == 0) {
        sdbError(edi, "Database '%s' is closed", sdb->edi.path);
        return 0;
    }
    if ((result = mprCreateList(0, MPR_LIST_STABLE)) == 0) {
        return 0;
    }
    defaultTableName = 0;
    rc = SQLITE_OK;
    nrows = 0;

    while (cmd && *cmd && (rc == SQLITE_OK || (rc == SQLITE_SCHEMA && ++retries < 2))) {
        stmt = 0;
        mprLog(2, "SQL: %s", cmd);
        rc = sqlite3_prepare_v2(db, cmd, -1, &stmt, &tail);
        if (rc != SQLITE_OK) {
            sdbTrace(edi, 2, "SDB: cannot prepare command: %s, error: %s", cmd, sqlite3_errmsg(db));
            continue;
        }
        if (stmt == 0) {
            /* Comment or white space */
            cmd = tail;
            continue;
        }
        if (vargs) {
            for (index = 0; (arg = va_arg(vargs, cchar*)) != 0; index++) {
                if (sqlite3_bind_text(stmt, index + 1, arg, -1, 0) != SQLITE_OK) {
                    sdbError(edi, "SDB: cannot bind to arg: %d, %s, error: %s", index + 1, arg, sqlite3_errmsg(db));
                    return 0;
                }
            }
        } else if (argv) {
            for (index = 0; index < argc; index++) {
                if (sqlite3_bind_text(stmt, index + 1, argv[index], -1, 0) != SQLITE_OK) {
                    sdbError(edi, "SDB: cannot bind to arg: %d, %s, error: %s", index + 1, argv[index], sqlite3_errmsg(db));
                    return 0;
                }
            } 
        }
        ncol = sqlite3_column_count(stmt);
        for (nrows = 0; ; nrows++) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                tableName = (char*) sqlite3_column_table_name(stmt, 0);
                if ((rec = createBareRec(edi, tableName, ncol)) == 0) {
                    sqlite3_finalize(stmt);
                    return 0;
                }
                if (defaultTableName == 0) {
                    defaultTableName = rec->tableName;
                }
                mprAddItem(result, rec);
                for (i = 0; i < ncol; i++) {
                    colName = sqlite3_column_name(stmt, i);
                    value = (cchar*) sqlite3_column_text(stmt, i);
                    if (tableName && strcmp(tableName, defaultTableName) != 0) {
                        len = strlen(tableName) + 1;
                        tableName = sjoin("_", tableName, colName, NULL);
                        tableName[len] = toupper((uchar) tableName[len]);
                    }
                    if (tableName && ((schema = getSchema(edi, tableName)) != 0)) {
                        rec->fields[i] = makeRecField(value, colName, schema->fields[i].type);
                    } else {
                        type = sqlite3_column_type(stmt, i);
                        rec->fields[i] = makeRecField(value, colName, mapSqliteTypeToEdiType(type));
                    }
                    if (smatch(colName, "id")) {
                        rec->fields[i].flags |= EDI_KEY;
                        rec->id = rec->fields[i].value;
                    }
                }
            } else {
                rc = sqlite3_finalize(stmt);
                stmt = 0;
                if (rc != SQLITE_SCHEMA) {
                    retries = 0;
                    for (cmd = tail; isspace((uchar) *cmd); cmd++) {}
                }
                break;
            }
        }
    }
    if (stmt) {
        rc = sqlite3_finalize(stmt);
    }
    if (rc != SQLITE_OK) {
        if (rc == sqlite3_errcode(db)) {
            sdbTrace(edi, 2, "SDB: cannot run query: %s, error: %s", cmd, sqlite3_errmsg(db));
        } else {
            sdbTrace(edi, 2, "SDB: unspecified SQL error for: %s", cmd);
        }
        return 0;
    }
    if ((grid = ediCreateBareGrid(edi, defaultTableName, nrows)) == 0) {
        return 0;
    }
    for (r = 0; r < nrows; r++) {
        grid->records[r] = mprGetItem(result, r);
    }
    return grid;
}


static bool validateField(Sdb *sdb, EdiRec *rec, cchar *tableName, cchar *columnName, cchar *value)
{
    EdiValidation   *vp;
    MprList         *validations;
    cchar           *error, *vkey;
    int             next;
    bool            pass;

    assert(sdb);
    assert(rec);
    assert(tableName && *tableName);
    assert(columnName && *columnName);

    pass = 1;
    vkey = sfmt("%s.%s", tableName, columnName);
    if ((validations = mprLookupKey(sdb->validations, vkey)) != 0) {
        for (ITERATE_ITEMS(validations, vp, next)) {
            if ((error = (*vp->vfn)(vp, rec, columnName, value)) != 0) {
                if (rec->errors == 0) {
                    rec->errors = mprCreateHash(0, MPR_HASH_STABLE);
                }
                mprAddKey(rec->errors, columnName, sfmt("%s %s", columnName, error));
                pass = 0;
            }
        }
    }
    return pass;
}


static EdiRec *createBareRec(Edi *edi, cchar *tableName, int nfields)
{
    EdiRec  *rec;

    if ((rec = mprAllocMem(sizeof(EdiRec) + sizeof(EdiField) * nfields, MPR_ALLOC_MANAGER | MPR_ALLOC_ZERO)) == 0) {
        return 0;
    }
    mprSetManager(rec, (MprManager) ediManageEdiRec);
    rec->edi = edi;
    rec->tableName = sclone(tableName);
    rec->nfields = nfields;
    return rec;
}


static EdiField makeRecField(cchar *value, cchar *name, int type)
{
    EdiField    f;

    f.valid = 1;
    f.value = sclone(value);
    f.name = sclone(name);
    f.type = type;
    f.flags = 0;
    return f;
}


static void *allocBlock(int size)
{
    void    *ptr;

    if ((ptr = mprAlloc(size)) != 0) {
        mprHold(ptr);
    }
    return ptr;
}


static void freeBlock(void *ptr)
{
    mprRelease(ptr);
}


static void *reallocBlock(void *ptr, int size)
{
    mprRelease(ptr);
    if ((ptr =  mprRealloc(ptr, size)) != 0) {
        mprHold(ptr);
    }
    return ptr;
}


static int blockSize(void *ptr)
{
    return (int) mprGetBlockSize(ptr);
}


static int roundBlockSize(int size)
{
    return MPR_ALLOC_ALIGN(size);
}


static int initAllocator(void *data)
{
    return 0;
}


static void termAllocator(void *data)
{
}


struct sqlite3_mem_methods mem = {
    allocBlock, freeBlock, reallocBlock, blockSize, roundBlockSize, initAllocator, termAllocator, NULL 
};


static cchar *mapToSqlType(int type)
{
    assert(0 < type && type < EDI_TYPE_MAX);
    return dataTypeToSqlType[type];
}


static int mapToEdiType(cchar *type)
{
    int     i;

    for (i = 0; i < EDI_TYPE_MAX; i++) {
        if (smatch(dataTypeToSqlType[i], type)) {
            return i;
        }
    }
    mprError("SDB: Cannot find type %s", type);
    assert(0);
    return 0;
}


static int mapSqliteTypeToEdiType(int type) 
{
    if (type == SQLITE_INTEGER) {
        return EDI_TYPE_INT;
    } else if (type == SQLITE_FLOAT) {
        return EDI_TYPE_FLOAT;
    } else if (type == SQLITE_TEXT) {
        return EDI_TYPE_TEXT;
    } else if (type == SQLITE_BLOB) {
        return EDI_TYPE_BINARY;
    } else if (type == SQLITE_NULL) {
        return EDI_TYPE_TEXT;
    }
    mprError("SDB: Cannot find query type %d", type);
    assert(0);
    return 0;
}


static bool validName(cchar *str)
{
    cchar   *cp;

    if (!str) {
        return 0;
    }
    if (!isalpha(*str) && *str != '_') {
        return 0;
    }
    for (cp = &str[1]; *cp && (isalnum((uchar) *cp) || *cp == '_' || *cp == '$'); cp++) {}
    return (*cp == '\0');
}


static void sdbError(Edi *edi, cchar *fmt, ...)
{
    va_list     args;

    va_start(args, fmt);
    edi->errMsg = sfmtv(fmt, args);
    va_end(args);
    mprError(edi->errMsg);
}


static void sdbTrace(Edi *edi, int level, cchar *fmt, ...)
{
    va_list     args;

    va_start(args, fmt);
    edi->errMsg = sfmtv(fmt, args);
    va_end(args);
    mprTrace(level, edi->errMsg);
}


/*********************************** Factory *******************************/

static void initSqlite()
{
    mprGlobalLock();
    if (!sqliteInitialized) {
        sqlite3_config(SQLITE_CONFIG_MALLOC, &mem);
        sqlite3_config(SQLITE_CONFIG_MULTITHREAD);
        if (sqlite3_initialize() != SQLITE_OK) {
            mprError("Cannot initialize SQLite");
            return;
        }
        sqliteInitialized = 1;
    }
    mprGlobalUnlock();
}

#else
/* To prevent ar/ranlib warnings */
PUBLIC void sdbDummy() {}
#endif /* BIT_PACK_ESP && BIT_PACK_SDB */

/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2013. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */
