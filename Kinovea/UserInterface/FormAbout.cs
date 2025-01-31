/*
Copyright © Joan Charmant 2008.
jcharmant@gmail.com 
 
This file is part of Kinovea.

Kinovea is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License version 2 
as published by the Free Software Foundation.

Kinovea is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Kinovea. If not, see http://www.gnu.org/licenses/.

*/

using Kinovea.Root.Languages;
using System;
using System.Diagnostics;
using System.Drawing;
using System.Resources;
using System.Threading;
using System.Windows.Forms;
using Kinovea.Services;
using System.Text;

namespace Kinovea.Root
{
    public partial class FormAbout : Form
    {
        private string year = "2019";
        private Font fontHeader = new Font("Microsoft Sans Serif", 10, FontStyle.Bold);
        private Font fontText = new Font("Microsoft Sans Serif", 9, FontStyle.Regular);

        public FormAbout()
        {
            InitializeComponent();
            Populate();
        }

        private void Populate()
        {
            this.Text = "   " + RootLang.mnuAbout;
            labelCopyright.Text = string.Format("Copyright © 2006-{0} - Joan Charmant and the Kinovea community.", year);
            lblKinovea.Text = string.Format("{0} - {1}", Software.ApplicationName, Software.Version);
            lnkKinovea.Links.Clear();
            lnkKinovea.Links.Add(0, lnkKinovea.Text.Length, "https://www.kinovea.org");

            PopulateTranslators();
            PopulateLicense();
            PopulateBuildingBlocks();
            PopulateCitation();
        }

        private void PopulateTranslators()
        {
            pageTranslation.Text = RootLang.dlgAbout_Translation;

            rtbTranslators.SelectionFont = fontText;
            rtbTranslators.AppendText(" " + LanguageManager.Dutch);
            AddTranslator("Peter Strikwerda, Bart Kerkvliet");
            rtbTranslators.AppendText(" " + LanguageManager.German);
            AddTranslator("Stephan Frost, Dominique Saussereau, Jonathan Boder, Stephan Peuckert");
            rtbTranslators.AppendText(" " + LanguageManager.Portuguese);
            AddTranslator("Fernando Jorge, Rafael Fernandes");
            rtbTranslators.AppendText(" " + LanguageManager.Spanish);
            AddTranslator("Rafael Gonzalez, Lionel Sosa Estrada, Andoni Morales Alastruey");
            rtbTranslators.AppendText(" " + LanguageManager.Italian);
            AddTranslator("Giorgio Biancuzzi");
            rtbTranslators.AppendText(" " + LanguageManager.Romanian);
            AddTranslator("Bogdan Paul Frăţilă");
            rtbTranslators.AppendText(" " + LanguageManager.Polish);
            AddTranslator("Kuba Zamojski");
            rtbTranslators.AppendText(" " + LanguageManager.Finnish);
            AddTranslator("Alexander Holthoer");
            rtbTranslators.AppendText(" " + LanguageManager.Norwegian);
            AddTranslator("Espen Kolderup");
            rtbTranslators.AppendText(" " + LanguageManager.Chinese);
            AddTranslator("Nicko Deng");
            rtbTranslators.AppendText(" " + LanguageManager.Turkish);
            AddTranslator("Eray Kıranoğlu");
            rtbTranslators.AppendText(" " + LanguageManager.Greek);
            AddTranslator("Nikos Sklavounos");
            rtbTranslators.AppendText(" " + LanguageManager.Lithuanian);
            AddTranslator("Mindaugas Slavikas");
            rtbTranslators.AppendText(" " + LanguageManager.Swedish);
            AddTranslator("Thomas Buska, Alexander Holthoer");
            rtbTranslators.AppendText(" " + LanguageManager.Danish);
            AddTranslator("Heinrich Winther");
            rtbTranslators.AppendText(" " + LanguageManager.Czech);
            AddTranslator("Jiří Rosický");
            rtbTranslators.AppendText(" " + LanguageManager.Korean);
            AddTranslator("RakJoon Sung");
            rtbTranslators.AppendText(" " + LanguageManager.Russian);
            AddTranslator("Andrey Pomerantsev");
            rtbTranslators.AppendText(" " + LanguageManager.Catalan);
            AddTranslator("Xavier Padullés");
            rtbTranslators.AppendText(" " + LanguageManager.Serbian);
            AddTranslator("Faculty of Sport and Phys. Ed. in Nis — Ivan Jovanovic, Mila Mladenovic, Rewea");
            rtbTranslators.AppendText(" " + LanguageManager.SerbianCyrl);
            AddTranslator("Sport's diagnostic center Sabac – Milan Djupovac");
            rtbTranslators.AppendText(" " + LanguageManager.Japanese);
            AddTranslator("Ryo Yamaguchi");
            rtbTranslators.AppendText(" " + LanguageManager.Macedonian);
            AddTranslator("Alexandar Aceski, Faculty of Physical Education - Skopje​");
            rtbTranslators.AppendText(" " + LanguageManager.Arabic);
            AddTranslator("Dr. Mansour Attaallah, Faculty of Physical Education Abu Qir, Alexandria University, Egypt​");
            rtbTranslators.AppendText(" " + LanguageManager.Farsi);
            AddTranslator("Moein Ansari");
        }
        
        private void AddTranslator(String translator)
        {
            rtbTranslators.SelectionColor = Color.SeaGreen;
            rtbTranslators.AppendText(String.Format(" : {0}.\n", translator));
            rtbTranslators.SelectionColor = Color.Black;
        }

        private void PopulateLicense()
        {
            pageLicense.Text = RootLang.dlgAbout_License;
        }

        private void PopulateBuildingBlocks()
        {
            pageBuildingBlocks.Text = RootLang.dlgAbout_BuildingBlocks;
         
            rtbBuildingBlocks.AppendText(" FFmpeg - Video formats and codecs - https://www.ffmpeg.org/\n");
            rtbBuildingBlocks.AppendText(" OpenCV - Computer Vision - http://opencv.org/.\n");
            rtbBuildingBlocks.AppendText(" AForge - Image processing - http://www.aforgenet.com/\n");
            rtbBuildingBlocks.AppendText(" EmguCV - OpenCV .NET Wrapper - http://www.emgu.com/\n");
            rtbBuildingBlocks.AppendText(" OxyPlot - Plotting. http://www.oxyplot.org/\n");
            rtbBuildingBlocks.AppendText(" Sharp Vector Graphics - http://sourceforge.net/projects/svgdomcsharp/\n");
            rtbBuildingBlocks.AppendText(" SharpZipLib - https://icsharpcode.github.io/SharpZipLib/.\n");
            rtbBuildingBlocks.AppendText(" ExpTree - Explorer Treeview - http://www.codeproject.com/Articles/8546/\n");
            rtbBuildingBlocks.AppendText(" FileDownloader - http://codeproject.com/cs/library/downloader.asp\n");
            rtbBuildingBlocks.AppendText(" log4Net - Logging utility. https://logging.apache.org/log4net/\n");
            rtbBuildingBlocks.AppendText(" Silk Icon set - http://www.famfamfam.com/lab/icons/silk/\n");
            rtbBuildingBlocks.AppendText(" Fugue Icon set - http://p.yusukekamiyamane.com/\n");
        }

        private void PopulateCitation()
        {
            pageCitation.Text = RootLang.dlgAbout_Citation;

            fontHeader = new Font("Microsoft Sans Serif", 9, FontStyle.Bold);
            fontText = new Font("Microsoft Sans Serif", 8.25f, FontStyle.Regular);

            rtbCitation.DetectUrls = false;

            rtbCitation.SelectionFont = fontHeader;
            rtbCitation.AppendText("BibTeX\n");

            rtbCitation.SelectionFont = fontText;
            StringBuilder b = new StringBuilder();
            b.AppendLine("@Misc{,");
            b.AppendLine("\ttitle = {Kinovea},");
            b.AppendLine("\tauthor = {Joan Charmant and contributors},");
            b.AppendLine(string.Format("\tyear = {{{0}}},", year));
            b.AppendLine("\turl = {http://www.kinovea.org}");
            b.AppendLine("}");
            rtbCitation.AppendText(b.ToString());
            rtbCitation.AppendText("\n");

            rtbCitation.SelectionFont = fontHeader;
            rtbCitation.AppendText("APA\n");
            rtbCitation.SelectionFont = fontText;
            rtbCitation.AppendText(string.Format("Charmant, ({0}) Kinovea (Version {1}) [Computer software]. Available from http://www.kinovea.org/\n", year, Software.Version));
            rtbCitation.AppendText("\n");

            rtbCitation.SelectionFont = fontHeader;
            rtbCitation.AppendText("MLA\n");
            rtbCitation.SelectionFont = fontText;
            rtbCitation.AppendText(string.Format("Charmant, Joan. Kinovea. Computer software. Kinovea. Vers. {0}. N.p., n.d. Web.\n", Software.Version));
            rtbCitation.AppendText("\n");

            rtbCitation.SelectionFont = fontHeader;
            rtbCitation.AppendText("Harvard\n");
            rtbCitation.SelectionFont = fontText;
            rtbCitation.AppendText(string.Format("Kinovea {0}, computer software {1}. Available from: <http://www.kinovea.org>.\n", Software.Version, year));
            rtbCitation.AppendText("\n");

        }

        private void lnkKinovea_LinkClicked(object sender, LinkLabelLinkClickedEventArgs e)
        {
            Process.Start(e.Link.LinkData.ToString());
        }

        private void rtbBuildingBlocks_LinkClicked(object sender, LinkClickedEventArgs e)
        {
            Process.Start(e.LinkText);
        }
    }
}