#ifndef __APPLE__

#include <stdlib.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <iostream>

#include <dwarf.h>
#include <libdwarf.h>

int LookupSource(Dwarf_Debug dbg, Dwarf_Die the_die, uint64_t target, char** sourceFile, uint32_t* lineNumber, uint32_t* column) {
	Dwarf_Line* lines = NULL;
	Dwarf_Error err;
	Dwarf_Signed lineCount = 0;

	// selection based on attributes low_pc and high_pc does not work correctly on FreeBSD 12.1 / GCC 9.4
  //fprintf(stderr, "LookupSource(%lx)\n", target);
	
	int retval = dwarf_srclines(the_die, &lines, &lineCount, &err);
	if (retval != DW_DLV_OK)
		// this entry has no source lines, maybe next entry does
		return 0;

	if (lineCount == 0)
		return 0;

	int match = -1;
  Dwarf_Addr matchDistance = 0;
	Dwarf_Addr prevlineaddr = 0;
	int prev = -1;
	for (int n = 0; n < lineCount; n++) {
		Dwarf_Addr lineaddr;
		if (dwarf_lineaddr(lines[n], &lineaddr, &err)) {
			prev = -1;
			continue;
		}
    //fprintf(stderr, "addr %llx\n", lineaddr);

    // if this is a close match, remember
    // close match = distance between start of segment and target addresss
		if (prev >= 0 && prevlineaddr <= target && target < lineaddr && (match == -1 || target - prevlineaddr < matchDistance)) {
			match = prev;
      matchDistance = target - prevlineaddr;
			//fprintf(stderr, "found match: %i. %llx - %llx\n", match, prevlineaddr, lineaddr);
      //break;
		}

		prevlineaddr = lineaddr;
		prev = n;
	}

	if (match >= 0 && match < lineCount-1) {
		/* Retrieve the line number in the source file. */
		Dwarf_Unsigned lineno;
		if (!dwarf_lineno(lines[match], &lineno, &err)) {

			/* Retrieve the file name for this descriptor. */
			char *filename;
			if (!dwarf_linesrc(lines[match], &filename, &err)) {

				//fprintf(stderr, "%s:%llu: (0x%08lx)\n", filename, lineno, target);
				*sourceFile = strdup(filename);
				*lineNumber = uint32_t(lineno);

        if (column) {
          Dwarf_Signed columnRaw;
          if (dwarf_lineoff(lines[match], &columnRaw, &err) == 0 && columnRaw >= 1) {
            *column = uint32_t(columnRaw);
          }
        }

				dwarf_dealloc(dbg,filename,DW_DLA_STRING);
			}
		}
	}

	dwarf_srclines_dealloc(dbg, lines, lineCount);
	return 0;
}

int LookupFunctionName(Dwarf_Die the_die, uint64_t target, char** functionName, uint32_t* offset) {
	assert(functionName);
	Dwarf_Error err;
	Dwarf_Half tag = 0;

	if (dwarf_tag(the_die, &tag, &err) != DW_DLV_OK)
		return -31;

	/* Only interested in subprogram DIEs here */
	if (tag != DW_TAG_subprogram)
		return 0;

	char* die_name = 0;
	int rc = dwarf_diename(the_die, &die_name, &err);
	if (rc == DW_DLV_ERROR)
		return -30;
	if (rc == DW_DLV_NO_ENTRY)
		return 0;

	/* Grab the DIEs attributes for display */
	Dwarf_Attribute* attrs = NULL;
	Dwarf_Signed attrcount;
	if (dwarf_attrlist(the_die, &attrs, &attrcount, &err) != DW_DLV_OK)
		return -33;

	Dwarf_Addr lowpc = 0, highpc = 0;
	for (Dwarf_Signed i = 0; i < attrcount; ++i) {
		Dwarf_Half attrcode;
		if (dwarf_whatattr(attrs[i], &attrcode, &err) != DW_DLV_OK)
			continue;

		/* We only take some of the attributes for display here.
		 ** More can be picked with appropriate tag constants.
		 */
		if (attrcode == DW_AT_low_pc) {
			Dwarf_Half form;
			dwarf_whatform(attrs[i], &form, &err);
			if (form == DW_FORM_addr) {
				dwarf_formaddr(attrs[i], &lowpc, &err);
			} else {
				abort();
			}
		} else if (attrcode == DW_AT_high_pc) {
			Dwarf_Half form;
			dwarf_whatform(attrs[i], &form, &err);
			if (form == DW_FORM_addr) {
				dwarf_formaddr(attrs[i], &highpc, &err);
			} else if (form == DW_FORM_udata || form == DW_FORM_data1 || form == DW_FORM_data2 || form == DW_FORM_data4 || form == DW_FORM_data8) {
				dwarf_formudata(attrs[i], &highpc, &err);
				highpc += lowpc;
			} else if (form == DW_FORM_sdata) {
				Dwarf_Signed s = 0;
				dwarf_formsdata(attrs[i], &s, &err);
				highpc = static_cast<Dwarf_Unsigned>(static_cast<Dwarf_Signed>(lowpc) + s);
			} else {
				abort();
			}
		}
	}

  // if (lowpc)
  //fprintf(stderr, "0x%08llx - 0x%08llx (%s)\n", lowpc, highpc, die_name);
	if (lowpc > target || !highpc || target >= highpc)
		return 0;

	*functionName = strdup(die_name);
	if (offset)
		*offset = uint32_t(target - lowpc);

	return 0;
}

int ProcessFile(Dwarf_Debug dbg, uint64_t target, char** sourceFile, uint32_t* lineNumber, uint32_t* column, char** functionName, uint32_t* offset)
{
  int rc = 0;
  if (functionName)
    *functionName = NULL;
  if (sourceFile)
    *sourceFile = NULL;
  Dwarf_Unsigned cu_header_length, abbrev_offset, next_cu_header = 0;
  Dwarf_Half version_stamp, address_size = 0;
  Dwarf_Error err;

  Dwarf_Die no_die = NULL;

  /* Find compilation unit header */
  while (rc == 0 &&
      ((sourceFile && !*sourceFile) || (functionName && !*functionName)) &&
      dwarf_next_cu_header(
        dbg,
        &cu_header_length,
        &version_stamp,
        &abbrev_offset,
        &address_size,
        &next_cu_header,
        &err) == DW_DLV_OK) {

    if (no_die != NULL)
      dwarf_dealloc(dbg, no_die, DW_DLA_DIE);
    no_die = 0;
    Dwarf_Die cu_die;
    Dwarf_Die child_die;
    // Expect the CU to have a single sibling - a DIE 
    while (dwarf_siblingof(dbg, no_die, &cu_die, &err) == DW_DLV_OK) {
      if (no_die != NULL)
        dwarf_dealloc(dbg, no_die, DW_DLA_DIE);
      no_die = cu_die;
      Dwarf_Half tag;
      if (dwarf_tag(no_die, &tag, &err) != DW_DLV_OK) {
        cu_die = nullptr;
        break;
      }

      // FIXME: DW_TAG_partial_unit??
      if (tag == DW_TAG_compile_unit)
        break;
    }
    if (!cu_die) {
      continue;
      //return -20;
    }

    if (sourceFile && !*sourceFile) {
      rc = LookupSource(dbg, cu_die, target, sourceFile, lineNumber, column);
    }

    if (functionName && !*functionName) {
      // Expect the CU DIE to have children 
      rc = dwarf_child(cu_die, &child_die, &err);
      if (rc != DW_DLV_OK) {
        rc = -21;
        break;
      }

      // Now go over all children DIEs
      while (functionName && !*functionName) {
        LookupFunctionName(child_die, target, functionName, offset);

        rc = dwarf_siblingof(dbg, child_die, &child_die, &err);
        if (rc == DW_DLV_ERROR) {
          rc = -22;
          break;
        } else if (rc == DW_DLV_NO_ENTRY)
          break; // done
      }
    }
  }

  if (no_die != NULL)
    dwarf_dealloc(dbg, no_die, DW_DLA_DIE);

  return rc;
}


int Lookup(const char* filename, uint64_t target, char** sourceFile, uint32_t* lineNumber, uint32_t* column, char** functionName, uint32_t* offset) {
	int fd = open(filename, O_RDONLY);

	if (fd < 0)
		return -1;

	Dwarf_Debug dbg = 0;
	Dwarf_Error err;
	if (dwarf_init(fd, DW_DLC_READ, 0, 0, &dbg, &err) != DW_DLV_OK) {
		return -2;
	}

	int rc = ProcessFile(dbg, target, sourceFile, lineNumber, column, functionName, offset);
	//fprintf(stderr, "look for source code info of %lx in %s -> %i (%s, %s)\n", target, filename, rc, sourceFile && *sourceFile ? *sourceFile : "-", functionName && *functionName ? *functionName : "-");

	if (dwarf_finish(dbg, &err) != DW_DLV_OK) {
		return -3;
	}

	close(fd);
	return rc;
}

#endif

