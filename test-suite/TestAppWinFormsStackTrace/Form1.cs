using System;
using System.Diagnostics;
using System.Windows.Forms;

namespace TestAppWinFormsStackTrace
{
    public sealed class Form1 : Form
    {
        public Form1()
        {
            this.Load += Form1_Load;
        }

        private void Form1_Load(object sender, EventArgs e)
        {
            var value = GetValue();
            Debug.WriteLine(value);
        }

        private string GetValue()
        {
            return "Hello World";
        }
    }
}
