#include <tcl.h>
#include <cstdio>
int main(int argc, char **argv) {
  if (argc != 3) return 2;
  Tcl_FindExecutable(argv[0]);
  Tcl_Interp *interp = Tcl_CreateInterp();
  Tcl_DString args;
  Tcl_DStringInit(&args);
  Tcl_DStringAppendElement(&args, argv[2]);
  Tcl_SetVar(interp, "argv", Tcl_DStringValue(&args), TCL_GLOBAL_ONLY);
  int status = Tcl_EvalFile(interp, argv[1]);
  if (status != TCL_OK) fprintf(stderr, "%s\n", Tcl_GetStringResult(interp));
  Tcl_DStringFree(&args);
  Tcl_DeleteInterp(interp);
  Tcl_Finalize();
  return status == TCL_OK ? 0 : 1;
}
