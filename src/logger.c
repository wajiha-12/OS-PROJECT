#include <stdio.h>

int main(int argc, char *argv[])
{
    const char *path = (argc>1) ? argv[1] : "schedule.log";
    FILE *out = fopen(path, "a");
   
    if (out == NULL) {
        perror("logger: cannot open log file");
        
        return 1;
    }

    char line[512];
    
    while (fgets(line, sizeof(line), stdin) != NULL) {
        fputs(line, out);
        fflush(out);
    }

    fclose(out);
    return 0;
}
