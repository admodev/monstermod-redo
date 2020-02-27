#include <stdio.h>
#include <string.h>
#include <ctype.h>

#ifndef __linux__
#include <io.h>
#else
#include <unistd.h>
#endif

#include "extdll.h"
#include "dllapi.h"
#include "meta_api.h"

#include "monster_plugin.h"

extern cvar_t *dllapi_log;

extern monster_type_t monster_types[];
extern int monster_spawn_count;


bool get_input(FILE *fp, char *input)
{
	char line[1024];
	int len, pos;

	while (!feof(fp))
	{
		if (fgets(line, 1023, fp) != NULL)
		{
			len = strlen(line);

			if (len == 0)
				continue;  // skip any null lines

			// remove any trailing newline, carriage return or whitespace...
			while ((line[len-1] == '\n') || (line[len-1] == '\r') || isspace(line[len-1]))
			{
				line[len-1] = 0;
				len--;
				if (len == 0)
					break;
			}

			pos = 0;

			while (isspace(line[pos]))
				pos++;  // skip leading blanks

			if ((line[pos] == '/') && (line[pos+1] == '/'))
				continue;  // skip comment lines

			if (line[pos] == 0)
				continue;  // skip empty lines

			strcpy(input, &line[pos]);
			return TRUE;
		}
	}

	return FALSE;  // no input found
}

struct pKVD
{
	char key[33];
	char value[33];
};
void scan_monster_cfg(FILE *fp)
{
	// Let's make a full rework of this. -Giegue
	char input[1024];
	float x, y, z;
	
	while (get_input(fp, input))
	{
		if (input[0] == '{')
		{
			// Proper start, initialize entity creation
			// Temporary variables to store entity data
			pKVD *data = (pKVD*)malloc(32*sizeof(*data)); // Entities should not have more than 32 keyvalues
			int kvd_index = 0;
			while (get_input(fp, input))
			{
				// It's the end of the entity structure?
				if (input[0] == '}')
				{
					// Done. Let's process the keyvalues.
					for (int i = 0; i < kvd_index; i++)
					{
						float x, y, z;
						// Any unknown keyvalue is ignored.
						// Any duplicate keyvalue is overwritten.
						
						if (strcmp(data[i].key, "origin") == 0)
						{
							if (sscanf(data[i].value, "%f %f %f", &x, &y, &z) != 3)
							{
								LOG_MESSAGE(PLID, "ERROR: invalid origin: %s", input); // print conflictive line
								
								// reset origin to g_vecZero
								LOG_MESSAGE(PLID, "ERROR: entity will spawn at 0 0 0");
								x = y = z = 0;
							}
							monster_spawnpoint[monster_spawn_count].origin[0] = x;
							monster_spawnpoint[monster_spawn_count].origin[1] = y;
							monster_spawnpoint[monster_spawn_count].origin[2] = z;
						}
						else if (strcmp(data[i].key, "delay") == 0)
						{
							// ToDo: Remove this keyvalue.
							// Monsters spawned directly should not respawn.
							if (sscanf(data[i].value, "%f", &x) != 1)
							{
								LOG_MESSAGE(PLID, "ERROR: invalid delay: %s", input); // print conflictive line
								
								// default to 30 seconds
								LOG_MESSAGE(PLID, "ERROR: entity respawn frequency will be set to 30 seconds");
								x = 30;
							}
							monster_spawnpoint[monster_spawn_count].delay = x;
						}
						else if (strcmp(data[i].key, "angles") == 0)
						{
							if (sscanf(data[i].value, "%f %f %f", &x, &y, &z) != 3)
							{
								LOG_MESSAGE(PLID, "ERROR: invalid angles: %s", input); // print conflictive line
								
								// reset angles to g_vecZero
								LOG_MESSAGE(PLID, "ERROR: entity angles will be set to 0 0 0");
								x = y = z = 0;
							}
							monster_spawnpoint[monster_spawn_count].angles[0] = x;
							monster_spawnpoint[monster_spawn_count].angles[1] = y;
							monster_spawnpoint[monster_spawn_count].angles[2] = z;
						}
						else if (strcmp(data[i].key, "classname") == 0)
						{
							int mIndex;
							for (mIndex = 0; monster_types[mIndex].name[0]; mIndex++)
							{
								if (strcmp(data[i].value, monster_types[mIndex].name) == 0)
								{
									monster_spawnpoint[monster_spawn_count].monster = mIndex;
									monster_types[mIndex].need_to_precache = TRUE;
									break;
								}
							}
							if (monster_types[mIndex].name[0] == 0)
							{
								LOG_MESSAGE(PLID, "ERROR: unknown classname: %s", input); // print conflictive line
								LOG_MESSAGE(PLID, "ERROR: nothing will spawn here!");
							}
						}
					}
					
					// Init monster
					monster_spawnpoint[monster_spawn_count].respawn_time = gpGlobals->time + 0.1; // spawn (nearly) right away
					monster_spawnpoint[monster_spawn_count].need_to_respawn = TRUE;
					
					// Log on? Print all the entities that were added
					if (dllapi_log->value)
					{
						// Classname only, or we will flood the server!
						// No, I'm not making this idiotproof. Classname should be the last KVD entry on an entity!
						LOG_CONSOLE(PLID, "[DEBUG] Added entity: %s", data[kvd_index-1].value);
					}
					
					monster_spawn_count++;
					free( data );
					break;
				}
				
				// Bruteforce to remove quotes
				char parse[66] = {0};
				int skip = 0;
				for (int i = 0; i < strlen(input); i++)
				{
					if (input[i] == '"')
					{
						skip++;
						continue;
					}
					parse[i-skip] = input[i];
				}
				parse[strlen(parse)] = '\0';
				
				// Copy all keyvalues to the tempvar
				// Key
				char *copy = strtok(parse, " ");
				strcpy(data[kvd_index].key, copy);
				
				// Value
				copy = strtok(NULL, " ");
				strcpy(data[kvd_index].value, "");
				while (copy != NULL)
				{
					// If the value is a vector, append necessary whitespaces
					strcat(data[kvd_index].value, copy);
					copy = strtok(NULL, " ");
					if (copy != NULL)
						strcat(data[kvd_index].value, " ");
				}
				
				// Next KVD
				kvd_index++;
			}
		}
	}
}

void process_monster_cfg(void)
{
	char game_dir[256];
	char filename[256];
	FILE *fp = NULL;
	bool status = FALSE;  // no error

	monster_spawn_count = 0;

	// find the directory name of the currently running MOD...
	(*g_engfuncs.pfnGetGameDir)(game_dir);

	strcpy(filename, game_dir);
#ifdef __linux__
	strcat(filename, "/maps/");
#else
	strcat(filename, "\\maps\\");
#endif
	strcat(filename, STRING(gpGlobals->mapname));
	strcat(filename, "_monster.cfg");

	// check if the map specific filename exists...
	if (access(filename, 0) == 0)
	{
		if (dllapi_log->value)
		{
			//META_CONS("[MONSTER] Processing config file=%s", filename);
			LOG_MESSAGE(PLID, "Processing config file=%s", filename);
		}

		if ((fp = fopen(filename, "r")) == NULL)
		{
			//META_CONS("[MONSTER] ERROR: Could not open \"%s\"!", filename);
			LOG_MESSAGE(PLID, "ERROR: Could not open \"%s\" file!", filename);

			return;
		}

		scan_monster_cfg(fp);

		fclose(fp);
	}

	return;
}


bool scan_monster_precache_cfg(FILE *fp)
{
	char input[1024];
	bool found;

	while (get_input(fp, input))
	{
		found = FALSE;

		for (int index=0; monster_types[index].name[0]; index++)
		{
			if (strcmp(input, monster_types[index].name) == 0)
			{
				monster_types[index].need_to_precache = TRUE;
				found = TRUE;
				break;
			}
		}

		if (found == FALSE)
		{
			//META_CONS("[MONSTER] ERROR: invalid precache monster name: %s", input);
			LOG_MESSAGE(PLID, "ERROR: invalid precache monster name: %s", input);
		}
	}

	return FALSE;
}


bool process_monster_precache_cfg(void)
{
	char game_dir[256];
	char filename[256];
	FILE *fp = NULL;
	bool status = FALSE;  // no error

	// find the directory name of the currently running MOD...
	(*g_engfuncs.pfnGetGameDir)(game_dir);

	strcpy(filename, game_dir);
	strcat(filename, "/monster_precache.cfg");

	// check if the map specific filename exists...
	if (access(filename, 0) == 0)
	{
		if (dllapi_log->value)
		{
			//META_CONS("[MONSTER] Processing config file=%s", filename);
			LOG_MESSAGE(PLID, "Processing config file=%s", filename);
		}

		if ((fp = fopen(filename, "r")) == NULL)
		{
			//META_CONS("[MONSTER] ERROR: Could not open \"%s\"!", filename);
			LOG_MESSAGE(PLID, "ERROR: Could not open \"%s\" file!", filename);

			return TRUE;  // return bad status
		}

		status = scan_monster_precache_cfg(fp);

		fclose(fp);
	}

	return status;
}